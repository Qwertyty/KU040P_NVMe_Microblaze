#include "xaxipcie.h"

XAxiPcie_Config XAxiPcie_ConfigTable[] __attribute__ ((section (".drvcfg_sec"))) = {

	{
		"xlnx,axi-pcie3-3.0", /* compatible */
		0x10000000, /* reg */
		0x2, /* xlnx,axibar-num */
		0x1, /* xlnx,include-baroffset-reg */
		0x1 /* xlnx,port-type */
	},
	 {
		 NULL
	}
};