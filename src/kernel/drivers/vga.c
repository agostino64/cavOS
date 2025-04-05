#include <fb.h>
#include <malloc.h>
#include <system.h>
#include <util.h>
#include <vga.h>

// The BIOS/UEFI GOP provided framebuffer... pretty darn basic
// Copyright (C) 2024 Panagiotis

static volatile struct limine_framebuffer_request limineFBreq = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0};

void initiateVGA() {
  struct limine_framebuffer *framebufferRes =
      limineFBreq.response->framebuffers[0];
  framebuffer = (uint8_t *)framebufferRes->address;
  framebufferHeight = framebufferRes->height;
  framebufferWidth = framebufferRes->width;
  framebufferPitch = framebufferRes->pitch;
  memcpy(&framebufferLimine, framebufferRes, sizeof(struct limine_framebuffer));
  debugf("[graphics] Resolution fixed: fb{%lx} dim(xy){%dx%d} bpp{%d}\n",
         framebuffer, framebufferWidth, framebufferHeight, framebufferRes->bpp);
}
