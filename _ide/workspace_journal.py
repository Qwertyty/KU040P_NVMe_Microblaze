# 2026-09-18T13:27:33.361714900
import vitis

client = vitis.create_client()
client.set_workspace(path="yscc_proj6")

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
status = comp.import_files(from_loc="$COMPONENT_LOCATION/../nvme", files=["source"], dest_dir_in_cmp = "xaxipcie_rc_enumerate_example", is_skip_copy_sources = False)

status = comp.import_files(from_loc="$COMPONENT_LOCATION/../nvme/source", files=["nvme_bench.c"], dest_dir_in_cmp = "src", is_skip_copy_sources = False)

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../source/xnvme.c", "../source/xnvmeha.c", "../source/xnvme_admin.c", "../source/xnvme_pal.c", "../../nvme/source/xnvme.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../source/xnvmeha.c", "../source/xnvme_admin.c", "../source/xnvme_pal.c", "../../nvme/source/xnvme.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../source/xnvme_admin.c", "../source/xnvme_pal.c", "../../nvme/source/xnvme.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../source/xnvme_pal.c", "../../nvme/source/xnvme.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../../nvme/source/xnvme.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../../nvme/source/xnvme_admin.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../../nvme/source/xnvme_pal.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c", "../../nvme/source/xnvmeha.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["nvme_bench.c"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = [""])

status = comp.import_files(from_loc="$COMPONENT_LOCATION/../nvme", files=["source"], dest_dir_in_cmp = "xaxipcie_rc_enumerate_example", is_skip_copy_sources = False)

platform = client.get_component(name="platform")
status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.set_app_config(key = "USER_INCLUDE_DIRECTORIES", values = ["../../nvme/include"])

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
status = comp.clean()

status = platform.build()

comp.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.set_app_config(key = "USER_INCLUDE_DIRECTORIES", values = ["../../nvme/include"])

comp.set_app_config(key = "USER_COMPILE_SOURCES", values = ["C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/nvme_bench.c", "C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvme.c", "C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvme_admin.c", "C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvme_pal.c", "C:/Users/qwerty/Desktop/yscc_proj6/nvme/source/xnvmeha.c"])

status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

client.delete_component(name="xaxipcie_rc_enumerate_example")

client.delete_component(name="xaxipcie_rc_enumerate_example")

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
status = comp.import_files(from_loc="$COMPONENT_LOCATION/../nvme", files=["source"], dest_dir_in_cmp = "xaxipcie_rc_enumerate_example", is_skip_copy_sources = False)

comp.set_app_config(key = "USER_INCLUDE_DIRECTORIES", values = ["C:/Users/qwerty/Desktop/yscc_proj6/nvme/include"])

status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

status = platform.build()

comp.build()

status = comp.clean()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

client.delete_component(name="xaxipcie_rc_enumerate_example")

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
status = comp.import_files(from_loc="$COMPONENT_LOCATION/../nvme", files=["source"], dest_dir_in_cmp = "xaxipcie_rc_enumerate_example", is_skip_copy_sources = False)

comp.set_app_config(key = "USER_INCLUDE_DIRECTORIES", values = ["C:/Users/qwerty/Desktop/yscc_proj6/nvme/include"])

status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = comp.clean()

