CFLAGS := -g -Wall $(CFLAGS)
LDFLAGS := -lpcap $(LDFLAGS)

targets = scheduler replay_pcap

.PHONY: all
all: $(targets)

scheduler: scheduler.o ts_pcap.o
replay_pcap: replay_pcap.o ts_pcap.o

.PHONY: clean
clean:
	$(RM) $(targets) *.o

