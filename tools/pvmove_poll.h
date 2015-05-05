/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LVM_PVMOVE_H
#define _LVM_PVMOVE_H

/* FIXME: remove it after refactoring completes */
#include <stdint.h>

struct cmd_context;
struct dm_list;
struct logical_volume;
struct volume_group;

struct volume_group *get_vg(struct cmd_context *cmd, const char *vgname);

int pvmove_update_metadata(struct cmd_context *cmd, struct volume_group *vg,
			   struct logical_volume *lv_mirr,
			   struct dm_list *lvs_changed, unsigned flags);

int pvmove_finish(struct cmd_context *cmd, struct volume_group *vg,
		  struct logical_volume *lv_mirr, struct dm_list *lvs_changed);

struct volume_group *pvmove_get_copy_vg(struct cmd_context *cmd,
					const char *name, const char *uuid,
					uint32_t flags);

#endif  /* _LVM_PVMOVE_H */
