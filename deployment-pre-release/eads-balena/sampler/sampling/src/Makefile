PROG = sampling

SRCS = hardware/mailbox.c hardware/virtual_memory.c hardware/system_timer.c hardware/dma.c hardware/pwm.c hardware/gpio.c hardware/spi.c hardware/clock.c dma_sampling.c socket_thread.c writer_thread.c offset_thread.c main.c
# NOTE: Here we need to disable optimization to prevent SIGBUS. This should be investigated.
CFLAGS = -Wall -Wextra -Wmissing-prototypes -Wimplicit -O1 -DSAMPLING_ADC=$(SAMPLING_ADC)
LDFLAGS = -Wl,--as-needed -pthread -latomic -lck -O1

.PHONY: all
all: $(PROG)

OBJS = $(SRCS:.c=.o)

.c.o:
	$(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

$(PROG): $(OBJS)
	$(CC) -o $@ $(OBJS) $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean:
	$(RM) $(PROG) $(OBJS)
