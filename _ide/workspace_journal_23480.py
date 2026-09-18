# 2026-09-18T11:51:16.219699
import vitis

client = vitis.create_client()
client.set_workspace(path="yscc_proj6")

platform = client.create_platform_component(name = "platform",hw_design = "$COMPONENT_LOCATION/../../top_ku040_add_timer.xsa",os = "standalone",cpu = "microblaze_0",domain_name = "standalone_microblaze_0",compiler = "gcc")

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
status = comp.import_files(from_loc="$COMPONENT_LOCATION/../nvme", files=["source"], dest_dir_in_cmp = "xaxipcie_rc_enumerate_example", is_skip_copy_sources = False)

comp.set_app_config(key = "USER_INCLUDE_DIRECTORIES", values = ["C:/Users/qwerty/Desktop/yscc_proj6/nvme/include"])

platform = client.get_component(name="platform")
status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

component = client.get_component(name="xaxipcie_rc_enumerate_example")

lscript = component.get_ld_script(path="C:\Users\qwerty\Desktop\yscc_proj6\xaxipcie_rc_enumerate_example\src\lscript.ld")

lscript.set_stack_size("0x4000")

status = platform.build()

comp.build()

status = platform.build()

comp.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
status = comp.import_files(from_loc="$COMPONENT_LOCATION/../nvme/source", files=["xnvme.c", "xnvme_admin.c", "xnvme_pal.c", "xnvmeha.c"], dest_dir_in_cmp = "nvme", is_skip_copy_sources = False)

status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["../source/xnvmeha.c", "../source/xnvme_admin.c", "../source/xnvme_pal.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["../source/xnvme_admin.c", "../source/xnvme_pal.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["../source/xnvme_pal.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = [""])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvme.c", "C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvme_admin.c", "C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvme_pal.c", "C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvmeha.c"])

status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["../../nvme/source/xnvme.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
status = comp.import_files(from_loc="$COMPONENT_LOCATION/src", files=["nvme_bench.c"], dest_dir_in_cmp = "src", is_skip_copy_sources = False)

status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

