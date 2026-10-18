CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function
CORE = fsu.o ppc.o pef.o emu.o text.o world.o hle_core.o hle_ae.o hle_files.o hle_scsi.o sim.o guard.o fwmem.o replay.o fghooks.o popups.o rawscan.o cassette.o
all: run_bg run_sys t_pef rawstack_test rawhdr_test rawhdr_local_test rawhdr_edge_test rawtile_test rawsr_test fwmem_test rawcmp_test rawsharp_test rawreg_test prefscopy_test rawchart_test cleangain_test streakcal_test negconv_test
%.o: %.c mac.h ppc.h pef.h sim.h fwmem.h ui_tables.h msys.h coro.h fsu.h cassette.h
	$(CC) $(CFLAGS) -c $< -o $@
run_bg: $(CORE) ui_stub.o hle_ui.o run_bg.o
	$(CC) $(CFLAGS) -o $@ $^ -lm
run_sys: $(CORE) ui_stub.o hle_ui.o coro.o msys.o run_sys.o
	$(CC) $(CFLAGS) -o $@ $^ -lm
rawstack_test: rawstack.c rawstack_test.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawstack_test.c rawstack.c -lm
rawhdr_edge_test: rawhdr.c rawhdr_edge_test.c rawhdr.h rawstack.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawhdr_edge_test.c rawhdr.c rawstack.c -lm
rawtile_test: rawtile.c rawtile_test.c rawtile.h rawstack.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawtile_test.c rawtile.c rawstack.c -lm
fwmem_test: fwmem.c fwmem_test.c fwmem.h
	$(CC) $(CFLAGS) -o $@ fwmem_test.c fwmem.c
rawsr_test: rawsr.c rawsr_test.c rawsr.h rawstack.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawsr_test.c rawsr.c rawstack.c -lm
rawcmp_test: rawcmp.c rawcmp_test.c rawcmp.h rawstack.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawcmp_test.c rawcmp.c rawstack.c -lm
rawhdr_local_test: rawhdr.c rawhdr_local_test.c rawhdr.h rawstack.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawhdr_local_test.c rawhdr.c rawstack.c -lm
rawhdr_test: rawhdr.c rawhdr_test.c rawhdr.h rawstack.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawhdr_test.c rawhdr.c rawstack.c -lm
t_pef: fsu.o pef.o t_pef.o
	$(CC) $(CFLAGS) -o $@ $^
clean:
	rm -f *.o run_bg t_pef
rawsharp_test: rawsharp.c rawsharp_test.c rawsharp.h
	$(CC) $(CFLAGS) -o $@ rawsharp_test.c rawsharp.c -lm
rawreg_test: rawreg.c rawreg_test.c rawreg.h rawstack.c rawstack.h
	$(CC) $(CFLAGS) -o $@ rawreg_test.c rawreg.c rawstack.c -lm

prefscopy_test: prefscopy.c prefscopy_test.c prefscopy.h text.c
	$(CC) $(CFLAGS) -o $@ prefscopy_test.c prefscopy.c text.c -lm

rawchart_test: rawchart.c rawchart_test.c rawchart.h
	$(CC) $(CFLAGS) -o $@ rawchart_test.c rawchart.c -lm

cleangain_test: cleangain.c cleangain_test.c cleangain.h
	$(CC) $(CFLAGS) -o $@ cleangain_test.c cleangain.c -lm

streakcal_test: streakcal.c streakcal_test.c streakcal.h cleangain.c cleangain.h
	$(CC) $(CFLAGS) -o $@ streakcal_test.c streakcal.c cleangain.c -lm

negconv_test: negconv.c negconv_test.c negconv.h
	$(CC) $(CFLAGS) -o $@ negconv_test.c negconv.c -lm
