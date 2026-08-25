#ifndef LORAITP_PORT_SIM_H
#define LORAITP_PORT_SIM_H

#include "loraitp_port.h"

#define LORAITP_SIM_MAX_FRAME 260

struct loraitp_sim;

void loraitp_sim_port(loraitp_port_t *port, struct loraitp_sim *sim);
void loraitp_sim_aes128(const uint8_t key[16], const uint8_t in[16],
                        uint8_t out[16]);
size_t loraitp_sim_size(void);
/* Returns NULL if mem_len < loraitp_sim_size(). */
struct loraitp_sim *loraitp_sim_new(void *mem, size_t mem_len, uint8_t *image,
                                    uint32_t image_len,
                                    const uint8_t key[16]);
uint32_t loraitp_sim_frames(const struct loraitp_sim *s);
void loraitp_sim_splice(struct loraitp_sim *dst, struct loraitp_sim *src);

#endif
