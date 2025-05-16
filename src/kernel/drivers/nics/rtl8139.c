#include <apic.h>
#include <bootloader.h>
#include <isr.h>
#include <malloc.h>
#include <rtl8139.h>
#include <system.h>

#include <util.h>

#include <paging.h>
#include <pmm.h>
#include <vmm.h>

// Realtek RTL8139 network card support (10/100Mbit)
// (as per https://wiki.osdev.org/RTL8139, clones may be different)
// Copyright (C) 2024 Panagiotis

// TODO! For legacy devices like these (with only 32-bit memory support), make
// some sort of memory-reservation interface to reserve lower addresses for
// usage - if (more like when) such a threshold is passed.

// NOTE! QEMU's RTL8139 implementation **for KVM accelerated guests** is is
// beyond horrible (so much so that even virt-manager hides that NIC away). For
// that reason the Intel e1000 should be a better option!

// Four TXAD register, you must use a different one to send packet each time(for
// example, use the first one, second... fourth and back to the first)
uint8_t TSAD_array[4] = {0x20, 0x24, 0x28, 0x2C};
uint8_t TSD_array[4] = {0x10, 0x14, 0x18, 0x1C};

bool isRTL8139(PCIdevice *device) {
  return (device->vendor_id == 0x10ec && device->device_id == 0x8139);
}

#define RTL8139_DEBUG 0
Spinlock LOCK_RTL8139 = {0};

void interruptHandler(AsmPassedInterrupt *regs) {
  rtl8139_interface *info = (rtl8139_interface *)selectedNIC->infoLocation;
  uint16_t           iobase = info->iobase;

  while (1) {
    uint16_t status = inportw(iobase + RTL8139_REG_ISR);

    outportw(info->iobase + RTL8139_REG_ISR, 0x5);

    if (!status)
      break;

    if (status & RTL8139_STATUS_TOK) {
#if RTL8139_DEBUG
      debugf("[pci::rtl8139] IRQ notification: Packet sent\n");
#endif
      for (int i = 0; i < 4; i++) {
        uint32_t in = inportl(iobase + TSD_array[i]);
        if (in & (1 << 15)) // transmit TOK xd
          info->tok |= (1 << i);
      }
    }
    if (status & RTL8139_STATUS_ROK) {
#if RTL8139_DEBUG
      debugf("[pci::rtl8139] IRQ notification: Processing packet...\n");
#endif
      // while (!(inportb(iobase + 0x37) & 0x01)) {
      receiveRTL8139(selectedNIC);
      // }
    }

    // if (status & (1 << 4)) {
    // }

    if (!(status & RTL8139_STATUS_TOK) && !(status & RTL8139_STATUS_ROK)) {
#if RTL8139_DEBUG
      debugf("[pci::rtl8139] IRQ notification: Unknown interrupt, status{%x}\n",
             status);
#endif
      break;
    }
  }
}

bool initiateRTL8139(PCIdevice *device) {
  if (!isRTL8139(device))
    return false;

  debugf("[pci::rtl8139] RTL-8139 NIC detected!\n");

  PCIgeneralDevice *details =
      (PCIgeneralDevice *)malloc(sizeof(PCIgeneralDevice));
  GetGeneralDevice(device, details);

  PCI *pci = lookupPCIdevice(device);
  setupPCIdeviceDriver(pci, PCI_DRIVER_RTL8139, PCI_DRIVER_CATEGORY_NIC);

  uint16_t iobase = details->bar[0] & ~0x3;

  NIC *nic = createNewNIC(pci);
  nic->type = RTL8139;
  nic->mintu = 60;
  nic->infoLocation = 0; // no extra info needed... yet.
  nic->irq = details->interruptLine;

  rtl8139_interface *infoLocation =
      (rtl8139_interface *)malloc(sizeof(rtl8139_interface));
  memset(infoLocation, 0, sizeof(rtl8139_interface));
  nic->infoLocation = infoLocation;

  infoLocation->iobase = iobase;
  infoLocation->tx_curr = 0; // init this

  // Enable PCI Bus Mastering if it's not enabled already
  uint32_t command_status = COMBINE_WORD(device->status, device->command);
  if (!(command_status & (1 << 2))) {
    command_status |= (1 << 2);
    ConfigWriteDword(device->bus, device->slot, device->function, PCI_COMMAND,
                     command_status);
  }

  // Turn the device on
  outportb(iobase + RTL8139_REG_POWERUP, 0x0);

  // Reset the device
  outportb(iobase + RTL8139_REG_CMD, 0x10);
  while ((inportb(iobase + RTL8139_REG_CMD) & 0x10) != 0) {
  }

  // Init the receive buffer (8192 + 16 + 1500)
  void *virtual = VirtualAllocatePhysicallyContiguous(
      DivRoundUp(8192 + 16 + 1500, BLOCK_SIZE));
  memset(virtual, 0, 8192 + 16 + 1500);
  size_t physical = VirtualToPhysical((size_t)virtual);
  outportl(iobase + RTL8139_REG_RBSTART, (uint32_t)physical);

  // Save it (physical can be computed if needed)
  infoLocation->rx_buff_virtual = virtual;
  debugf("[pci::rtl8139] RX buffer allocated: virtual{%lx} physical{%lx}\n",
         virtual, physical);

  // Set the TOK and ROK bits high
  outportw(iobase + RTL8139_REG_IMR, 0x0005);

  // Order of the last two IO ports matters..
  // https://forum.osdev.org/viewtopic.php?f=1&t=33107

  // Sets the RE and TE bits high
  outportb(iobase + RTL8139_REG_CMD, 0x0C);

  // (1 << 7) is the WRAP bit, 0xf is AB+AM+APM+AAP
  outportl(iobase + 0x44, 0xf | (1 << 7));

  uint32_t MAC0_5 = inportl(iobase + RTL8139_REG_MAC0_5);
  uint16_t MAC5_6 = inportw(iobase + RTL8139_REG_MAC5_6);
  nic->MAC[0] = MAC0_5 >> 0;
  nic->MAC[1] = MAC0_5 >> 8;
  nic->MAC[2] = MAC0_5 >> 16;
  nic->MAC[3] = MAC0_5 >> 24;

  nic->MAC[4] = MAC5_6 >> 0;
  nic->MAC[5] = MAC5_6 >> 8;

  memcpy(nic->ip, defaultIP, 4);

  // waste of memory:
  // debugf(
  //        "[pci::ne2k] MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
  //        selectedNIC->MAC[0], selectedNIC->MAC[1], selectedNIC->MAC[2],
  //        selectedNIC->MAC[3], selectedNIC->MAC[4], selectedNIC->MAC[5]);

  uint8_t targIrq = ioApicPciRegister(device, details);
  free(details);
  pci->irqHandler = registerIRQhandler(targIrq, &interruptHandler);

  // Solve QEMU's weird fiddleness by "kindly" reminding it to wake up our
  // device!
  outportb(iobase + RTL8139_REG_POWERUP, 0x0);
  outportb(iobase + RTL8139_REG_POWERUP, 0x0);
  outportb(iobase + RTL8139_REG_POWERUP, 0x0);

  return true;
}

void sendRTL8139(NIC *nic, void *packet, uint32_t packetSize) {
  spinlockAcquire(&LOCK_RTL8139);
  rtl8139_interface *info = (rtl8139_interface *)nic->infoLocation;
  uint16_t           iobase = info->iobase;

  void *contiguousContainer =
      VirtualAllocatePhysicallyContiguous(DivRoundUp(packetSize, BLOCK_SIZE));
  size_t phys = VirtualToPhysical((size_t)contiguousContainer);
  if (phys > (UINT32_MAX - 0x5000)) {
    VirtualFree(contiguousContainer, DivRoundUp(packetSize, BLOCK_SIZE));
    debugf("[pci::rtl8139] FATAL! Ran out of 32-bit addresses!\n");
    return;
  }
  memcpy(contiguousContainer, packet, packetSize);

  uint8_t tx_active = info->tx_curr;

  outportl(iobase + TSAD_array[info->tx_curr], (uint32_t)(phys));
  outportl(iobase + TSD_array[info->tx_curr++], packetSize);
  if (info->tx_curr > 3)
    info->tx_curr = 0;

  while (!(inportl(iobase + TSD_array[tx_active]) & (1 << 15)))
    ; // transmit TOK xd

  // while (!(info->tok & (1 << tx_active)))
  //   ;

  outportl(iobase + TSD_array[tx_active], 0x2000);

  info->tok &= ~(1 << tx_active);

  VirtualFree(contiguousContainer, DivRoundUp(packetSize, BLOCK_SIZE));
  spinlockRelease(&LOCK_RTL8139);
}

void receiveRTL8139(NIC *nic) {
  rtl8139_interface *info = (rtl8139_interface *)nic->infoLocation;
  uint16_t           iobase = info->iobase;

  while ((inportb(iobase + 0x37) & 0x01) == 0) {
    uint16_t *buffer =
        (uint16_t *)(info->rx_buff_virtual + info->currentPacket);
    uint16_t packetStatus = *(buffer);
    uint16_t packetLength = *(buffer + 1);
    if (!packetStatus || packetStatus == 0xe1e3) {
      debugf("[pci::rtl8139] FATAL! Bad packet status{%x}!\n", packetStatus);
      return;
    }

    // we don't need the packet's pointer & length
    buffer += 2;

    netQueueAdd(nic, (uint8_t *)buffer, packetLength - 4); // remove control bit

    // to be removed:
    // debugf("WE GOT A PACKET!\n");
    // for (int i = 0; i < packetLength; i++)
    //   debugf("%02X ", ((uint8_t *)packet)[i]);

    info->currentPacket = (info->currentPacket + packetLength + 4 + 3) & (~3);
    if (info->currentPacket >= 8192)
      info->currentPacket -= 8192;

    outportw(iobase + 0x38, info->currentPacket - 0x10);
  }
}
