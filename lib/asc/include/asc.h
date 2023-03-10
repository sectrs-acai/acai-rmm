/*
 * SPDX-License-Identifier: BSD-3-Clause
 * SPDX-FileCopyrightText: Copyright TF-RMM Contributors.
 */
#ifndef ASC_H
#define ASC_H

void asc_mark_secure(unsigned long addr);
void asc_mark_nonsecure(unsigned long addr);
void asc_mark_secure_dev(unsigned long addr, unsigned long delegate_flag, unsigned long iova);
void asc_add_translation_table(unsigned long phys_addr,unsigned long iova, unsigned int sid);
void asc_attach_dev(unsigned long addr);

#endif /* ASC_H */
