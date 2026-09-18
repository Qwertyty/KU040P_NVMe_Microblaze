set(ddr4_0_memory_0 "0x80000000;0x80000000")
set(microblaze_0_local_memory_dlmb_bram_if_cntlr_memory_0 "0x50;0x1fb0")
set(DDR ddr4_0_memory_0)
set(CODE ddr4_0_memory_0)
set(DATA ddr4_0_memory_0)
set(TOTAL_MEM_CONTROLLERS "ddr4_0_memory_0;microblaze_0_local_memory_dlmb_bram_if_cntlr_memory_0")
set(MEMORY_SECTION "MEMORY
{
	ddr4_0_memory_0 : ORIGIN = 0x80000000, LENGTH = 0x80000000
	microblaze_0_local_memory_dlmb_bram_if_cntlr_memory_0 : ORIGIN = 0x50, LENGTH = 0x1fb0
}")
set(STACK_SIZE 0x400)
set(HEAP_SIZE 0x400)
