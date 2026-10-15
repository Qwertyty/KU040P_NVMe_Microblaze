# 2026-09-21T09:24:59.304528500
import vitis

client = vitis.create_client()
client.set_workspace(path="yscc_proj6")

platform = client.get_component(name="platform")
status = platform.build()

comp = client.get_component(name="xaxipcie_rc_enumerate_example")
comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = comp.clean()

vitis.dispose()

