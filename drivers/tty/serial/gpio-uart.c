// SPDX-License-Identifier: GPL-2.0
/*
 * Software UART implemented over two GPIOs, running on the Xenomai 3 /
 * Dovetail out-of-band (OOB) execution stage for deterministic bit timing.
 *
 * The bit-level engine (TX shifting, RX sampling and start-bit detection)
 * runs entirely on the real-time core using rtdm_timer / rtdm_irq and direct
 * (raw) GPIO register access, so it is never preempted by the in-band Linux
 * kernel. The standard tty/serial layer is in-band only, therefore received
 * bytes and TX refill/wakeup are handed over to the in-band stage through
 * rtdm_nrtsig deferred work.
 *
 * Only 8N1 is supported. GPIOs must be MMIO (non-sleeping) and active-high
 * (idle line = high, start bit = low), as the engine uses gpiod_*_raw_value().
 *
 * Validated operating envelope (Cobalt clock gravity calibrated via autotune):
 *   - up to 19200 baud: stable under heavy system load (half-bit budget ~26us,
 *     well above the worst-case OOB timer jitter observed on this SoC);
 *   - 38400 baud: reliable at idle / light-to-moderate load (half-bit budget
 *     ~13us, which sits close to the worst-case jitter under extreme load).
 * Higher rates are intentionally rejected (see gpio_uart_valid_baud()): their
 * half-bit budget drops below the achievable jitter floor, so framing cannot
 * be guaranteed without dedicated hardware. For guaranteed 38400 under heavy
 * load, isolate a CPU for the RT core (isolcpus/nohz_full) and pin the RX IRQ
 * to it.
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/idr.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/slab.h>
#include <linux/tty_flip.h>

#include <rtdm/driver.h>
#include <cobalt/kernel/timer.h>	/* xntimer_set_gravity(), XNTIMER_KGRAVITY */

#define GPIO_UART_NAME			"gpio-uart"
#define GPIO_UART_DEV_NAME		"ttyGU"
#define GPIO_UART_NR_PORTS		4
#define GPIO_UART_DEFAULT_BAUD		19200	/* guaranteed-stable default */
#define GPIO_UART_MIN_BAUD		2400
#define GPIO_UART_MAX_BAUD		38400	/* hard ceiling; see header comment */
#define GPIO_UART_BITS_PER_FRAME	10	/* 1 start + 8 data + 1 stop */
#define GPIO_UART_RX_SAMPLE_PERCENT	50	/* sample at mid-bit by default */

/* Power-of-two sized SPSC byte rings shared across stages (rtdm_lock guarded). */
#define GPIO_UART_TX_RING_SIZE		256
#define GPIO_UART_RX_RING_SIZE		256
#define GPIO_UART_TX_RING_MASK		(GPIO_UART_TX_RING_SIZE - 1)
#define GPIO_UART_RX_RING_MASK		(GPIO_UART_RX_RING_SIZE - 1)

struct gpio_uart_port {
	struct uart_port port;

	struct gpio_desc *tx_gpio;
	struct gpio_desc *rx_gpio;
	int rx_irq_no;

	/*
	 * OOB engine synchronisation. This is a hard spinlock usable from both
	 * the in-band stage (rtdm_lock_get_irqsave) and the OOB stage
	 * (rtdm_lock_get). It protects every field below plus both rings.
	 */
	rtdm_lock_t lock;

	rtdm_irq_t rx_irqh;
	bool rx_irq_taken;
	rtdm_timer_t tx_timer;
	rtdm_timer_t rx_timer;
	bool timers_ready;

	/* OOB -> in-band deferred work. */
	rtdm_nrtsig_t tx_sig;
	rtdm_nrtsig_t rx_sig;

	unsigned int baud;
	u64 bit_ns;
	u32 rx_sample_percent;

	/* TX engine: in-band producer (tx ring) / OOB consumer. */
	bool tx_running;
	bool tx_stop;
	u16 tx_frame;
	unsigned int tx_bit;
	u8 tx_ring[GPIO_UART_TX_RING_SIZE];
	unsigned int tx_head;	/* in-band producer */
	unsigned int tx_tail;	/* OOB consumer */

	/* RX engine: OOB producer (rx ring) / in-band consumer. */
	bool rx_enabled;
	bool rx_running;
	unsigned int rx_sample;	/* 0=start, 1..8=data, 9=stop */
	u8 rx_byte;
	u8 rx_ring_ch[GPIO_UART_RX_RING_SIZE];
	u8 rx_ring_ferr[GPIO_UART_RX_RING_SIZE];
	unsigned int rx_head;	/* OOB producer */
	unsigned int rx_tail;	/* in-band consumer */

	/* OOB timer jitter instrumentation (working deadlines + stats). */
	u64 tx_deadline;
	u64 rx_deadline;

	struct dentry *debugfs_dir;
	u64 tx_frames;
	u64 rx_frames;
	u64 rx_frame_err;
	u64 rx_false_start;
	u64 rx_overrun;
	u64 tx_timer_late_max_ns;
	u64 rx_timer_late_max_ns;
	u64 tx_late_count;
	u64 rx_late_count;
};

static DEFINE_IDA(gpio_uart_ida);

static struct uart_driver gpio_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= GPIO_UART_NAME,
	.dev_name	= GPIO_UART_DEV_NAME,
	.nr		= GPIO_UART_NR_PORTS,
};

static inline struct gpio_uart_port *to_gpio_uart_port(struct uart_port *port)
{
	return container_of(port, struct gpio_uart_port, port);
}

/*
 * Only standard rates up to GPIO_UART_MAX_BAUD (38400) are accepted. Faster
 * rates are rejected on purpose: their half-bit sampling budget falls below
 * the OOB timer jitter floor, so reliable framing cannot be guaranteed.
 */
static bool gpio_uart_valid_baud(unsigned int baud)
{
	switch (baud) {
	case 2400:
	case 4800:
	case 9600:
	case 19200:
	case 38400:
		return true;
	default:
		return false;
	}
}

static void gpio_uart_set_baud(struct gpio_uart_port *gup, unsigned int baud)
{
	if (!gpio_uart_valid_baud(baud))
		baud = GPIO_UART_DEFAULT_BAUD;

	gup->baud = baud;
	gup->bit_ns = DIV_ROUND_CLOSEST_ULL(NSEC_PER_SEC, baud);
}

/*
 * Record OOB timer lateness (actual fire time - scheduled deadline). Called
 * from the OOB handlers with gup->lock held. This is the bit-clock jitter that
 * must stay well below one bit period for reliable framing.
 */
static void gpio_uart_record_late(struct gpio_uart_port *gup, u64 *max_ns,
				  u64 *late_cnt, s64 late_ns)
{
	if (late_ns <= 0)
		return;

	if ((u64)late_ns > *max_ns)
		*max_ns = (u64)late_ns;

	if ((u64)late_ns > gup->bit_ns / 2)
		(*late_cnt)++;
}

static u64 gpio_uart_rx_first_offset_ns(struct gpio_uart_port *gup)
{
	u32 percent = READ_ONCE(gup->rx_sample_percent);

	if (percent < 10 || percent > 90)
		percent = GPIO_UART_RX_SAMPLE_PERCENT;

	return div_u64(gup->bit_ns * percent, 100);
}

/* --- TX ring (in-band producer, OOB consumer) --- */

static inline bool gpio_uart_tx_ring_empty(struct gpio_uart_port *gup)
{
	return gup->tx_head == gup->tx_tail;
}

static inline bool gpio_uart_tx_ring_full(struct gpio_uart_port *gup)
{
	return (gup->tx_head - gup->tx_tail) >= GPIO_UART_TX_RING_SIZE;
}

/* Pop one byte and build its 8N1 frame. Caller holds gup->lock. */
static bool gpio_uart_tx_load_frame(struct gpio_uart_port *gup)
{
	u8 ch;

	if (gpio_uart_tx_ring_empty(gup))
		return false;

	ch = gup->tx_ring[gup->tx_tail & GPIO_UART_TX_RING_MASK];
	gup->tx_tail++;

	/* bit0 = start (0), bit1..8 = data LSB..MSB, bit9 = stop (1). */
	gup->tx_frame = BIT(9) | (ch << 1);
	gup->tx_bit = 0;
	gup->tx_frames++;

	return true;
}

/*
 * Refill the TX ring from the tty circular buffer. Runs in-band with
 * port->lock held; returns true if the ring holds at least one byte.
 */
static bool gpio_uart_tx_refill(struct gpio_uart_port *gup)
{
	struct uart_port *port = &gup->port;
	struct circ_buf *xmit;
	rtdm_lockctx_t ctx;
	bool empty;

	if (!port->state)
		return false;

	xmit = &port->state->xmit;

	rtdm_lock_get_irqsave(&gup->lock, ctx);

	if (port->x_char && !gpio_uart_tx_ring_full(gup)) {
		gup->tx_ring[gup->tx_head & GPIO_UART_TX_RING_MASK] = port->x_char;
		gup->tx_head++;
		port->x_char = 0;
		port->icount.tx++;
	}

	while (!gpio_uart_tx_ring_full(gup)) {
		if (uart_circ_empty(xmit) || uart_tx_stopped(port))
			break;

		gup->tx_ring[gup->tx_head & GPIO_UART_TX_RING_MASK] =
			xmit->buf[xmit->tail];
		gup->tx_head++;
		uart_xmit_advance(port, 1);
	}

	empty = gpio_uart_tx_ring_empty(gup);

	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	return !empty;
}

/*
 * Start (or keep feeding) the OOB TX engine. Runs in-band with port->lock
 * held. The engine is armed but every bit (start included) is driven from the
 * OOB timer handler, so the whole frame shares one gravity-compensated time
 * base with uniform inter-bit spacing.
 */
static void gpio_uart_tx_try_start(struct gpio_uart_port *gup)
{
	rtdm_lockctx_t ctx;
	bool arm = false;
	u64 base = 0;

	if (!gpio_uart_tx_refill(gup))
		return;

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	gup->tx_stop = false;
	if (gup->timers_ready && !gup->tx_running &&
	    gpio_uart_tx_load_frame(gup)) {
		gup->tx_running = true;
		base = rtdm_clock_read_monotonic();
		gup->tx_deadline = base + gup->bit_ns;
		arm = true;
	}
	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	if (arm)
		rtdm_timer_start(&gup->tx_timer, gup->tx_deadline,
				 gup->bit_ns, RTDM_TIMERMODE_ABSOLUTE);
}

/* OOB periodic TX bit clock. */
static void gpio_uart_tx_handler(rtdm_timer_t *timer)
{
	struct gpio_uart_port *gup =
		container_of(timer, struct gpio_uart_port, tx_timer);
	bool notify = false;
	int level;

	rtdm_lock_get(&gup->lock);

	if (!gup->tx_running) {
		rtdm_lock_put(&gup->lock);
		return;
	}

	gpio_uart_record_late(gup, &gup->tx_timer_late_max_ns,
			      &gup->tx_late_count,
			      (s64)(rtdm_clock_read_monotonic() - gup->tx_deadline));
	gup->tx_deadline += gup->bit_ns;

	if (gup->tx_bit >= GPIO_UART_BITS_PER_FRAME) {
		/* Previous stop bit has been held for one bit period. */
		if (gup->tx_stop || !gpio_uart_tx_load_frame(gup)) {
			gpiod_set_raw_value(gup->tx_gpio, 1);	/* idle high */
			gup->tx_running = false;
			gup->tx_stop = false;
			rtdm_timer_stop_in_handler(&gup->tx_timer);
			rtdm_lock_put(&gup->lock);
			rtdm_nrtsig_pend(&gup->tx_sig);
			return;
		}
		notify = true;
	}

	level = (gup->tx_frame >> gup->tx_bit) & 1;
	gpiod_set_raw_value(gup->tx_gpio, level);
	gup->tx_bit++;

	rtdm_lock_put(&gup->lock);

	if (notify)
		rtdm_nrtsig_pend(&gup->tx_sig);
}

/* In-band: refill ring, wake writers, restart engine if it drained. */
static void gpio_uart_tx_nrt(rtdm_nrtsig_t *sig, void *arg)
{
	struct gpio_uart_port *gup = arg;
	struct uart_port *port = &gup->port;
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	gpio_uart_tx_try_start(gup);
	spin_unlock_irqrestore(&port->lock, flags);
}

/* --- RX ring (OOB producer, in-band consumer) --- */

static inline bool gpio_uart_rx_ring_full(struct gpio_uart_port *gup)
{
	return (gup->rx_head - gup->rx_tail) >= GPIO_UART_RX_RING_SIZE;
}

/* OOB start-bit detector. */
static int gpio_uart_rx_irq(rtdm_irq_t *irqh)
{
	struct gpio_uart_port *gup =
		rtdm_irq_get_arg(irqh, struct gpio_uart_port);
	bool arm = false;

	/*
	 * Data-bit transitions during a frame re-enter this handler. We do
	 * not mask the line (xnintr_enable/disable are no-ops on this
	 * pipeline, and returning RTDM_IRQ_DISABLE would call the sleeping
	 * disable_irq() from OOB context); the rx_running guard discards
	 * those spurious edges instead.
	 */
	rtdm_lock_get(&gup->lock);
	if (gup->rx_enabled && gup->timers_ready && !gup->rx_running &&
	    gpiod_get_raw_value(gup->rx_gpio) == 0) {
		gup->rx_running = true;
		gup->rx_sample = 0;
		gup->rx_byte = 0;
		gup->rx_deadline = rtdm_clock_read_monotonic() +
				   gpio_uart_rx_first_offset_ns(gup);
		arm = true;
	}
	rtdm_lock_put(&gup->lock);

	if (arm)
		rtdm_timer_start(&gup->rx_timer, gup->rx_deadline,
				 gup->bit_ns, RTDM_TIMERMODE_ABSOLUTE);

	return RTDM_IRQ_HANDLED;
}

/* OOB periodic RX sampler (mid-bit). */
static void gpio_uart_rx_handler(rtdm_timer_t *timer)
{
	struct gpio_uart_port *gup =
		container_of(timer, struct gpio_uart_port, rx_timer);
	bool finish = false;
	bool push = false;
	unsigned int idx;
	int level;

	rtdm_lock_get(&gup->lock);

	if (!gup->rx_running) {
		rtdm_lock_put(&gup->lock);
		return;
	}

	gpio_uart_record_late(gup, &gup->rx_timer_late_max_ns,
			      &gup->rx_late_count,
			      (s64)(rtdm_clock_read_monotonic() - gup->rx_deadline));
	gup->rx_deadline += gup->bit_ns;

	level = gpiod_get_raw_value(gup->rx_gpio);
	idx = gup->rx_sample;

	if (idx == 0) {
		if (level) {			/* line went high: false start */
			gup->rx_false_start++;
			finish = true;
		} else {
			gup->rx_sample = 1;
		}
	} else if (idx <= 8) {
		if (level)
			gup->rx_byte |= BIT(idx - 1);
		gup->rx_sample++;
	} else {				/* idx == 9: stop bit */
		bool ferr = !level;

		if (ferr)
			gup->rx_frame_err++;

		if (!gpio_uart_rx_ring_full(gup)) {
			gup->rx_ring_ch[gup->rx_head & GPIO_UART_RX_RING_MASK] =
				gup->rx_byte;
			gup->rx_ring_ferr[gup->rx_head & GPIO_UART_RX_RING_MASK] =
				ferr;
			gup->rx_head++;
			gup->rx_frames++;
			push = true;
		} else {
			gup->rx_overrun++;
		}
		finish = true;
	}

	if (finish) {
		gup->rx_running = false;
		gup->rx_sample = 0;
		rtdm_timer_stop_in_handler(&gup->rx_timer);
	}

	rtdm_lock_put(&gup->lock);

	/*
	 * No IRQ re-enable needed: the line was never masked. Once rx_running
	 * is cleared above, the next start edge (falling) re-arms reception.
	 */
	if (push)
		rtdm_nrtsig_pend(&gup->rx_sig);
}

/* In-band: drain RX ring into the tty layer. */
static void gpio_uart_rx_nrt(rtdm_nrtsig_t *sig, void *arg)
{
	struct gpio_uart_port *gup = arg;
	struct uart_port *port = &gup->port;
	bool pushed = false;

	if (!port->state)
		return;

	for (;;) {
		rtdm_lockctx_t ctx;
		bool empty;
		u8 ch = 0;
		u8 ferr = 0;

		rtdm_lock_get_irqsave(&gup->lock, ctx);
		empty = (gup->rx_tail == gup->rx_head);
		if (!empty) {
			ch = gup->rx_ring_ch[gup->rx_tail & GPIO_UART_RX_RING_MASK];
			ferr = gup->rx_ring_ferr[gup->rx_tail & GPIO_UART_RX_RING_MASK];
			gup->rx_tail++;
		}
		rtdm_lock_put_irqrestore(&gup->lock, ctx);

		if (empty)
			break;

		port->icount.rx++;
		if (ferr) {
			port->icount.frame++;
			uart_insert_char(port, UART_LSR_FE, 0, ch, TTY_FRAME);
		} else {
			uart_insert_char(port, 0, 0, ch, TTY_NORMAL);
		}
		pushed = true;
	}

	if (pushed)
		tty_flip_buffer_push(&port->state->port);
}

/* --- uart_ops --- */

static unsigned int gpio_uart_tx_empty(struct uart_port *port)
{
	struct gpio_uart_port *gup = to_gpio_uart_port(port);
	rtdm_lockctx_t ctx;
	bool empty;

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	empty = !gup->tx_running && gpio_uart_tx_ring_empty(gup);
	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	return empty ? TIOCSER_TEMT : 0;
}

static void gpio_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static unsigned int gpio_uart_get_mctrl(struct uart_port *port)
{
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void gpio_uart_stop_tx(struct uart_port *port)
{
	struct gpio_uart_port *gup = to_gpio_uart_port(port);
	rtdm_lockctx_t ctx;

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	gup->tx_stop = true;
	rtdm_lock_put_irqrestore(&gup->lock, ctx);
}

static void gpio_uart_start_tx(struct uart_port *port)
{
	gpio_uart_tx_try_start(to_gpio_uart_port(port));
}

static void gpio_uart_stop_rx(struct uart_port *port)
{
	struct gpio_uart_port *gup = to_gpio_uart_port(port);
	rtdm_lockctx_t ctx;

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	gup->rx_enabled = false;
	gup->rx_running = false;
	gup->rx_sample = 0;
	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	if (gup->timers_ready)
		rtdm_timer_stop(&gup->rx_timer);
}

static void gpio_uart_start_rx(struct uart_port *port)
{
	struct gpio_uart_port *gup = to_gpio_uart_port(port);
	rtdm_lockctx_t ctx;

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	gup->rx_enabled = true;
	gup->rx_running = false;
	gup->rx_sample = 0;
	rtdm_lock_put_irqrestore(&gup->lock, ctx);
}

static int gpio_uart_startup(struct uart_port *port)
{
	struct gpio_uart_port *gup = to_gpio_uart_port(port);
	rtdm_lockctx_t ctx;
	int ret;

	gpiod_set_raw_value(gup->tx_gpio, 1);	/* idle high */

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	gup->tx_running = false;
	gup->tx_stop = false;
	gup->tx_head = gup->tx_tail = 0;
	gup->rx_running = false;
	gup->rx_enabled = true;
	gup->rx_sample = 0;
	gup->rx_head = gup->rx_tail = 0;
	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	ret = rtdm_timer_init(&gup->tx_timer, gpio_uart_tx_handler,
			      "gpio_uart_tx");
	if (ret)
		return ret;

	ret = rtdm_timer_init(&gup->rx_timer, gpio_uart_rx_handler,
			      "gpio_uart_rx");
	if (ret)
		goto err_tx_timer;

	/*
	 * Use kernel gravity so the core programs the hardware timer early
	 * enough to compensate for the latency down to our OOB handler. This
	 * removes the systematic ~irq-gravity lateness that rtdm_timer_init()
	 * leaves in place and is what brings worst-case bit jitter below the
	 * budget required for the higher baud rates.
	 */
	xntimer_set_gravity(&gup->tx_timer, XNTIMER_KGRAVITY);
	xntimer_set_gravity(&gup->rx_timer, XNTIMER_KGRAVITY);

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	gup->timers_ready = true;
	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	irq_set_irq_type(gup->rx_irq_no, IRQ_TYPE_EDGE_FALLING);
	ret = rtdm_irq_request(&gup->rx_irqh, gup->rx_irq_no, gpio_uart_rx_irq,
			       0, dev_name(port->dev), gup);
	if (ret) {
		dev_err(port->dev,
			"failed to request OOB rx irq %d (%d); the GPIO irqchip may not support out-of-band delivery\n",
			gup->rx_irq_no, ret);
		goto err_rx_timer;
	}

	gup->rx_irq_taken = true;
	rtdm_irq_enable(&gup->rx_irqh);

	return 0;

err_rx_timer:
	gup->timers_ready = false;
	rtdm_timer_destroy(&gup->rx_timer);
err_tx_timer:
	rtdm_timer_destroy(&gup->tx_timer);
	return ret;
}

static void gpio_uart_shutdown(struct uart_port *port)
{
	struct gpio_uart_port *gup = to_gpio_uart_port(port);
	rtdm_lockctx_t ctx;
	bool was_ready;

	/*
	 * Close the arming gate under the lock first: once timers_ready is
	 * cleared, gpio_uart_tx_try_start()/gpio_uart_rx_irq() can no longer
	 * (re)arm the timers, so it is safe to free the IRQ and destroy them.
	 */
	rtdm_lock_get_irqsave(&gup->lock, ctx);
	was_ready = gup->timers_ready;
	gup->timers_ready = false;
	gup->rx_enabled = false;
	gup->rx_running = false;
	gup->tx_stop = true;
	gup->tx_running = false;
	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	if (gup->rx_irq_taken) {
		rtdm_irq_free(&gup->rx_irqh);
		gup->rx_irq_taken = false;
	}

	if (was_ready) {
		rtdm_timer_destroy(&gup->tx_timer);
		rtdm_timer_destroy(&gup->rx_timer);
	}

	gpiod_set_raw_value(gup->tx_gpio, 1);
}

static void gpio_uart_set_termios(struct uart_port *port,
				  struct ktermios *termios,
				  const struct ktermios *old)
{
	struct gpio_uart_port *gup = to_gpio_uart_port(port);
	unsigned int baud;
	unsigned long flags;
	rtdm_lockctx_t ctx;
	bool busy;

	termios->c_cflag &= ~(PARENB | PARODD | CMSPAR | CSTOPB | CSIZE |
			      CRTSCTS);
	termios->c_cflag |= CS8 | CREAD | CLOCAL;

	baud = uart_get_baud_rate(port, termios, old,
				 GPIO_UART_MIN_BAUD, GPIO_UART_MAX_BAUD);
	if (!gpio_uart_valid_baud(baud))
		baud = GPIO_UART_DEFAULT_BAUD;

	spin_lock_irqsave(&port->lock, flags);

	rtdm_lock_get_irqsave(&gup->lock, ctx);
	busy = gup->tx_running || gup->rx_running;

	if (busy && baud != READ_ONCE(gup->baud))
		baud = READ_ONCE(gup->baud);
	else
		gpio_uart_set_baud(gup, baud);

	baud = gup->baud;
	rtdm_lock_put_irqrestore(&gup->lock, ctx);

	port->uartclk = baud * 16;
	uart_update_timeout(port, termios->c_cflag, baud);

	spin_unlock_irqrestore(&port->lock, flags);

	tty_termios_encode_baud_rate(termios, baud, baud);
}

static const char *gpio_uart_type(struct uart_port *port)
{
	return port->type == PORT_UARTLITE ? GPIO_UART_NAME : NULL;
}

static void gpio_uart_release_port(struct uart_port *port)
{
}

static int gpio_uart_request_port(struct uart_port *port)
{
	return 0;
}

static void gpio_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_UARTLITE;
}

static int gpio_uart_verify_port(struct uart_port *port,
				 struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_UARTLITE)
		return -EINVAL;

	return 0;
}

static const struct uart_ops gpio_uart_ops = {
	.tx_empty	= gpio_uart_tx_empty,
	.set_mctrl	= gpio_uart_set_mctrl,
	.get_mctrl	= gpio_uart_get_mctrl,
	.stop_tx	= gpio_uart_stop_tx,
	.start_tx	= gpio_uart_start_tx,
	.stop_rx	= gpio_uart_stop_rx,
	.start_rx	= gpio_uart_start_rx,
	.startup	= gpio_uart_startup,
	.shutdown	= gpio_uart_shutdown,
	.set_termios	= gpio_uart_set_termios,
	.type		= gpio_uart_type,
	.release_port	= gpio_uart_release_port,
	.request_port	= gpio_uart_request_port,
	.config_port	= gpio_uart_config_port,
	.verify_port	= gpio_uart_verify_port,
};

static void gpio_uart_debugfs_init(struct gpio_uart_port *gup)
{
	char name[16];

	snprintf(name, sizeof(name), "%s%d", GPIO_UART_DEV_NAME, gup->port.line);
	gup->debugfs_dir = debugfs_create_dir(name, NULL);
	if (IS_ERR_OR_NULL(gup->debugfs_dir))
		return;

	debugfs_create_u64("tx_frames", 0444, gup->debugfs_dir,
			   &gup->tx_frames);
	debugfs_create_u64("rx_frames", 0444, gup->debugfs_dir,
			   &gup->rx_frames);
	debugfs_create_u64("rx_frame_err", 0444, gup->debugfs_dir,
			   &gup->rx_frame_err);
	debugfs_create_u64("rx_false_start", 0444, gup->debugfs_dir,
			   &gup->rx_false_start);
	debugfs_create_u64("rx_overrun", 0444, gup->debugfs_dir,
			   &gup->rx_overrun);
	debugfs_create_u64("tx_timer_late_max_ns", 0444, gup->debugfs_dir,
			   &gup->tx_timer_late_max_ns);
	debugfs_create_u64("rx_timer_late_max_ns", 0444, gup->debugfs_dir,
			   &gup->rx_timer_late_max_ns);
	debugfs_create_u64("tx_late_count", 0444, gup->debugfs_dir,
			   &gup->tx_late_count);
	debugfs_create_u64("rx_late_count", 0444, gup->debugfs_dir,
			   &gup->rx_late_count);
	debugfs_create_u32("rx_sample_percent", 0644, gup->debugfs_dir,
			   &gup->rx_sample_percent);
}

static int gpio_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_uart_port *gup;
	u32 baud = GPIO_UART_DEFAULT_BAUD;
	int ret;
	int line;

	line = ida_alloc_max(&gpio_uart_ida, GPIO_UART_NR_PORTS - 1, GFP_KERNEL);
	if (line < 0)
		return line;

	gup = devm_kzalloc(dev, sizeof(*gup), GFP_KERNEL);
	if (!gup) {
		ret = -ENOMEM;
		goto err_ida_free;
	}

	gup->tx_gpio = devm_gpiod_get(dev, "tx", GPIOD_OUT_HIGH);
	if (IS_ERR(gup->tx_gpio)) {
		ret = dev_err_probe(dev, PTR_ERR(gup->tx_gpio),
				    "failed to get tx GPIO\n");
		goto err_ida_free;
	}

	gup->rx_gpio = devm_gpiod_get(dev, "rx", GPIOD_IN);
	if (IS_ERR(gup->rx_gpio)) {
		ret = dev_err_probe(dev, PTR_ERR(gup->rx_gpio),
				    "failed to get rx GPIO\n");
		goto err_ida_free;
	}

	if (gpiod_cansleep(gup->tx_gpio) || gpiod_cansleep(gup->rx_gpio)) {
		dev_err(dev, "sleeping GPIOs are not supported\n");
		ret = -EINVAL;
		goto err_ida_free;
	}

	gup->rx_irq_no = gpiod_to_irq(gup->rx_gpio);
	if (gup->rx_irq_no < 0) {
		ret = dev_err_probe(dev, gup->rx_irq_no,
				    "failed to map rx GPIO IRQ\n");
		goto err_ida_free;
	}

	device_property_read_u32(dev, "current-speed", &baud);
	gpio_uart_set_baud(gup, baud);
	gup->rx_sample_percent = GPIO_UART_RX_SAMPLE_PERCENT;

	spin_lock_init(&gup->port.lock);
	rtdm_lock_init(&gup->lock);
	rtdm_nrtsig_init(&gup->tx_sig, gpio_uart_tx_nrt, gup);
	rtdm_nrtsig_init(&gup->rx_sig, gpio_uart_rx_nrt, gup);

	gup->port.dev = dev;
	gup->port.line = line;
	gup->port.type = PORT_UARTLITE;
	gup->port.iotype = UPIO_MEM;
	gup->port.irq = gup->rx_irq_no;
	gup->port.fifosize = 1;
	gup->port.flags = UPF_BOOT_AUTOCONF | UPF_FIXED_PORT;
	gup->port.ops = &gpio_uart_ops;
	gup->port.uartclk = gup->baud * 16;

	platform_set_drvdata(pdev, gup);

	ret = uart_add_one_port(&gpio_uart_driver, &gup->port);
	if (ret)
		goto err_nrtsig;

	gpio_uart_debugfs_init(gup);

	dev_info(dev, "registered %s%d at %u baud (OOB/Xenomai engine)\n",
		 GPIO_UART_DEV_NAME, line, gup->baud);

	return 0;

err_nrtsig:
	rtdm_nrtsig_destroy(&gup->tx_sig);
	rtdm_nrtsig_destroy(&gup->rx_sig);
err_ida_free:
	ida_free(&gpio_uart_ida, line);
	return ret;
}

static int gpio_uart_remove(struct platform_device *pdev)
{
	struct gpio_uart_port *gup = platform_get_drvdata(pdev);

	debugfs_remove_recursive(gup->debugfs_dir);
	uart_remove_one_port(&gpio_uart_driver, &gup->port);
	rtdm_nrtsig_destroy(&gup->tx_sig);
	rtdm_nrtsig_destroy(&gup->rx_sig);
	ida_free(&gpio_uart_ida, gup->port.line);

	return 0;
}

static const struct of_device_id gpio_uart_of_match[] = {
	{ .compatible = "cix,gpio-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_uart_of_match);

static struct platform_driver gpio_uart_platform_driver = {
	.probe	= gpio_uart_probe,
	.remove	= gpio_uart_remove,
	.driver	= {
		.name		= GPIO_UART_NAME,
		.of_match_table	= gpio_uart_of_match,
	},
};

static int __init gpio_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&gpio_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&gpio_uart_platform_driver);
	if (ret)
		uart_unregister_driver(&gpio_uart_driver);

	return ret;
}
module_init(gpio_uart_init);

static void __exit gpio_uart_exit(void)
{
	platform_driver_unregister(&gpio_uart_platform_driver);
	uart_unregister_driver(&gpio_uart_driver);
	ida_destroy(&gpio_uart_ida);
}
module_exit(gpio_uart_exit);

MODULE_DESCRIPTION("Software UART over GPIO (Xenomai OOB real-time engine)");
MODULE_AUTHOR("Jelly Jia <Jelly.Jia@cixtech.com>");
MODULE_LICENSE("GPL");
