#ifndef AHCI_H
#define AHCI_H

/* ahci : find the SATA (AHCI) controller and list the devices on its ports. See docs/ahci.md. */
int cmd_ahci(int argc, char **argv);

#endif
