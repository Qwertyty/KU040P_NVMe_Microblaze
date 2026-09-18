
cmake_minimum_required(VERSION 3.15)
project(bsp)

# Use CMAKE_LIBRARY_PATH in link_directories
link_directories(${CMAKE_LIBRARY_PATH})
        
set(DRIVER_TARGETS axidma;intc;axipcie;tmrctr;uartlite;bram)
set(DRIVER_LOCATIONS D:/soft/xilinx/2025.2.1/Vitis/data/embeddedsw/XilinxProcessorIPLib/drivers/axidma_v9_20;D:/soft/xilinx/2025.2.1/Vitis/data/embeddedsw/XilinxProcessorIPLib/drivers/intc_v3_21;D:/soft/xilinx/2025.2.1/Vitis/data/embeddedsw/XilinxProcessorIPLib/drivers/axipcie_v3_4;D:/soft/xilinx/2025.2.1/Vitis/data/embeddedsw/XilinxProcessorIPLib/drivers/tmrctr_v4_14;D:/soft/xilinx/2025.2.1/Vitis/data/embeddedsw/XilinxProcessorIPLib/drivers/uartlite_v3_12;D:/soft/xilinx/2025.2.1/Vitis/data/embeddedsw/XilinxProcessorIPLib/drivers/bram_v4_13)

list(LENGTH DRIVER_TARGETS no_of_drivers)
set(index 0)

while(${index} LESS ${no_of_drivers})
    list(GET DRIVER_TARGETS ${index} drv)
    list(GET DRIVER_LOCATIONS ${index} drv_dir)
    set(exp_dir "${drv_dir}/examples")
    add_custom_target(
        ${drv}_example ALL
        COMMAND lopper -f -O C:/Users/qwerty/Desktop/yscc_proj6/platform/microblaze_0/standalone_microblaze_0/bsp/libsrc/${drv} C:/Users/qwerty/Desktop/yscc_proj6/platform/microblaze_0/standalone_microblaze_0/bsp/hw_artifacts/microblaze_0_baremetal.dts -- bmcmake_metadata_xlnx microblaze_0 ${exp_dir} drvcmake_metadata
        BYPRODUCTS x${drv}_exlist.yaml)
    MATH(EXPR index "${index}+1")
endwhile()
