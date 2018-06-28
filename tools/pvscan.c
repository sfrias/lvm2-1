/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"

#include "lib/cache/lvmetad.h"
#include "lib/cache/lvmcache.h"
#include "lib/metadata/metadata.h"

#include <dirent.h>

static int _pvscan_online_aa(struct cmd_context *cmd, int argc, char **argv);

struct pvscan_params {
	int new_pvs_found;
	int pvs_found;
	uint64_t size_total;
	uint64_t size_new;
	unsigned pv_max_name_len;
	unsigned vg_max_name_len;
	unsigned pv_tmp_namelen;
	char *pv_tmp_name;
};

struct pvscan_aa_params {
	int refresh_all;
	unsigned int activate_errors;
	struct dm_list changed_vgnames;
};

static int _pvscan_display_pv(struct cmd_context *cmd,
				  struct physical_volume *pv,
				  struct pvscan_params *params)
{
	/* XXXXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXXXX */
	char uuid[40] __attribute__((aligned(8)));
	const unsigned suffix_len = sizeof(uuid) + 10;
	unsigned pv_len;
	const char *pvdevname = pv_dev_name(pv);

	/* short listing? */
	if (arg_is_set(cmd, short_ARG)) {
		log_print_unless_silent("%s", pvdevname);
		return ECMD_PROCESSED;
	}

	if (!params->pv_max_name_len) {
		lvmcache_get_max_name_lengths(cmd, &params->pv_max_name_len, &params->vg_max_name_len);

		params->pv_max_name_len += 2;
		params->vg_max_name_len += 2;
		params->pv_tmp_namelen = params->pv_max_name_len + suffix_len;

		if (!(params->pv_tmp_name = dm_pool_alloc(cmd->mem, params->pv_tmp_namelen)))
			return ECMD_FAILED;
	}

	pv_len = params->pv_max_name_len;
	memset(params->pv_tmp_name, 0, params->pv_tmp_namelen);

	if (arg_is_set(cmd, uuid_ARG)) {
		if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
			stack;
			return ECMD_FAILED;
		}

		if (dm_snprintf(params->pv_tmp_name, params->pv_tmp_namelen, "%-*s with UUID %s",
				params->pv_max_name_len - 2, pvdevname, uuid) < 0) {
			log_error("Invalid PV name with uuid.");
			return ECMD_FAILED;
		}
		pvdevname = params->pv_tmp_name;
		pv_len += suffix_len;
	}

	if (is_orphan(pv))
		log_print_unless_silent("PV %-*s    %-*s %s [%s]",
					pv_len, pvdevname,
					params->vg_max_name_len, " ",
					pv->fmt ? pv->fmt->name : "    ",
					display_size(cmd, pv_size(pv)));
	else if (pv_status(pv) & EXPORTED_VG)
		log_print_unless_silent("PV %-*s  is in exported VG %s [%s / %s free]",
					pv_len, pvdevname, pv_vg_name(pv),
					display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
					display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
	else
		log_print_unless_silent("PV %-*s VG %-*s %s [%s / %s free]",
					pv_len, pvdevname,
					params->vg_max_name_len, pv_vg_name(pv),
					pv->fmt ? pv->fmt->name : "    ",
					display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
					display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
	return ECMD_PROCESSED;
}

static int _pvscan_display_single(struct cmd_context *cmd, struct volume_group *vg,
			  struct physical_volume *pv, struct processing_handle *handle)
{
	struct pvscan_params *params = (struct pvscan_params *)handle->custom_handle;

	if ((arg_is_set(cmd, exported_ARG) && !(pv_status(pv) & EXPORTED_VG)) ||
	    (arg_is_set(cmd, novolumegroup_ARG) && (!is_orphan(pv)))) {
		return ECMD_PROCESSED;

	}

	params->pvs_found++;

	if (is_orphan(pv)) {
		params->new_pvs_found++;
		params->size_new += pv_size(pv);
		params->size_total += pv_size(pv);
	} else {
		params->size_total += (uint64_t) pv_pe_count(pv) * pv_pe_size(pv);
	}

	_pvscan_display_pv(cmd, pv, params);
	return ECMD_PROCESSED;
}

static int _lvmetad_clear_dev(dev_t devno, int32_t major, int32_t minor)
{
	char buf[24];

	(void) dm_snprintf(buf, sizeof(buf), FMTd32 ":" FMTd32, major, minor);

	if (!lvmetad_pv_gone(devno, buf))
		return_0;

	log_print_unless_silent("Device %s not found. Cleared from lvmetad cache.", buf);

	return 1;
}

/*
 * pvscan --cache does not perform any lvmlockd locking, and
 * pvscan --cache -aay skips autoactivation in lockd VGs.
 *
 * pvscan --cache populates lvmetad with VG metadata from disk.
 * No lvmlockd locking is needed.  It is expected that lockd VG
 * metadata that is read by pvscan and populated in lvmetad may
 * be immediately stale due to changes to the VG from other hosts
 * during or after this pvscan.  This is normal and not a problem.
 * When a subsequent lvm command uses the VG, it will lock the VG
 * with lvmlockd, read the VG from lvmetad, and update the cached
 * copy from disk if necessary.
 *
 * pvscan --cache -aay does not activate LVs in lockd VGs because
 * activation requires locking, and a lock-start operation is needed
 * on a lockd VG before any locking can be performed in it.
 *
 * An equivalent of pvscan --cache -aay for lockd VGs is:
 * 1. pvscan --cache
 * 2. vgchange --lock-start
 * 3. vgchange -aay -S 'locktype=sanlock || locktype=dlm'
 *
 * [We could eventually add support for autoactivating lockd VGs
 * using pvscan by incorporating the lock start step (which can
 * take a long time), but there may be a better option than
 * continuing to overload pvscan.]
 * 
 * Stages of starting a lockd VG:
 *
 * . pvscan --cache populates lockd VGs in lvmetad without locks,
 *   and this initial cached copy may quickly become stale.
 *
 * . vgchange --lock-start VG reads the VG without the VG lock
 *   because no locks are available until the locking is started.
 *   It only uses the VG name and lock_type from the VG metadata,
 *   and then only uses it to start the VG lockspace in lvmlockd.
 *
 * . Further lvm commands, e.g. activation, can then lock the VG
 *   with lvmlockd and use current VG metdata.
 */

#define REFRESH_BEFORE_AUTOACTIVATION_RETRIES 5
#define REFRESH_BEFORE_AUTOACTIVATION_RETRY_USLEEP_DELAY 100000

static int _pvscan_autoactivate_single(struct cmd_context *cmd, const char *vg_name,
				       struct volume_group *vg, struct processing_handle *handle)
{
	struct pvscan_aa_params *pp = (struct pvscan_aa_params *)handle->custom_handle;
	unsigned int refresh_retries = REFRESH_BEFORE_AUTOACTIVATION_RETRIES;
	int refresh_done = 0;

	if (vg_is_clustered(vg))
		return ECMD_PROCESSED;

	if (vg_is_exported(vg))
		return ECMD_PROCESSED;

	if (vg_is_shared(vg))
		return ECMD_PROCESSED;

	log_debug("pvscan autoactivating VG %s.", vg_name);

	/*
	 * Refresh LVs in a VG that has "changed" from finding a PV.
	 * The meaning of "changed" is determined in lvmetad, and is
	 * returned to the command as a flag.
	 *
	 * FIXME: There's a tiny race when suspending the device which is part
	 * of the refresh because when suspend ioctl is performed, the dm
	 * kernel driver executes (do_suspend and dm_suspend kernel fn):
	 *
	 *          step 1: a check whether the dev is already suspended and
	 *                  if yes it returns success immediately as there's
	 *                  nothing to do
	 *          step 2: it grabs the suspend lock
	 *          step 3: another check whether the dev is already suspended
	 *                  and if found suspended, it exits with -EINVAL now
	 *
	 * The race can occur in between step 1 and step 2. To prevent premature
	 * autoactivation failure, we're using a simple retry logic here before
	 * we fail completely. For a complete solution, we need to fix the
	 * locking so there's no possibility for suspend calls to interleave
	 * each other to cause this kind of race.
	 *
	 * Remove this workaround with "refresh_retries" once we have proper locking in!
	 */
	if (pp->refresh_all || str_list_match_item(&pp->changed_vgnames, vg_name)) {
		while (refresh_retries--) {
			log_debug_activation("Refreshing VG %s before autoactivation.", vg_name);
			if (vg_refresh_visible(cmd, vg)) {
				refresh_done = 1;
				break;
			}
			usleep(REFRESH_BEFORE_AUTOACTIVATION_RETRY_USLEEP_DELAY);
		}

		if (!refresh_done)
			log_warn("%s: refresh before autoactivation failed.", vg->name);
	}

	log_debug_activation("Autoactivating VG %s.", vg_name);

	if (!vgchange_activate(cmd, vg, CHANGE_AAY)) {
		log_error("%s: autoactivation failed.", vg->name);
		pp->activate_errors++;
		goto out;
	}

out:
	return ECMD_PROCESSED;
}

static int _pvscan_autoactivate(struct cmd_context *cmd, struct pvscan_aa_params *pp,
				int all_vgs, struct dm_list *vgnames)
{
	struct processing_handle *handle = NULL;
	int ret;

	if (!all_vgs && dm_list_empty(vgnames)) {
		log_debug("No VGs to autoactivate.");
		return ECMD_PROCESSED;
	}

	if (!lvmetad_used())
		log_warn("WARNING: Autoactivation reading from disk instead of lvmetad.");

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = pp;

	if (all_vgs) {
		cmd->cname->flags |= ALL_VGS_IS_DEFAULT;
		pp->refresh_all = 1;
	}

	ret = process_each_vg(cmd, 0, NULL, NULL, vgnames, READ_FOR_UPDATE, 0, handle, _pvscan_autoactivate_single);

	destroy_processing_handle(cmd, handle);

	return ret;
}

static int _pvscan_cache(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_aa_params pp = { 0 };
	struct dm_list single_devs;
	struct dm_list found_vgnames;
	struct device *dev;
	struct device_list *devl;
	const char *pv_name;
	const char *reason = NULL;
	int32_t major = -1;
	int32_t minor = -1;
	int devno_args = 0;
	struct arg_value_group_list *current_group;
	dev_t devno;
	int do_activate;
	int all_vgs = 0;
	int remove_errors = 0;
	int add_errors = 0;
	int ret = ECMD_PROCESSED;

	dm_list_init(&found_vgnames);
	dm_list_init(&pp.changed_vgnames);

	if ((do_activate = arg_is_set(cmd, activate_ARG))) {
		if (arg_uint_value(cmd, activate_ARG, 0) != CHANGE_AAY) {
			log_error("Only --activate ay allowed with pvscan.");
			return EINVALID_CMD_LINE;
		}

		if (!lvmetad_used() &&
		    !find_config_tree_bool(cmd, global_use_lvmetad_CFG, NULL)) {
			log_verbose("Ignoring pvscan --cache -aay because lvmetad is not in use.");
			return ret;
		}
	} else {
		if (!lvmetad_used()) {
			log_verbose("Ignoring pvscan --cache because lvmetad is not in use.");
			return ret;
		}
	}

	if (arg_is_set(cmd, major_ARG) + arg_is_set(cmd, minor_ARG))
		devno_args = 1;

	if (devno_args && (!arg_is_set(cmd, major_ARG) || !arg_is_set(cmd, minor_ARG))) {
		log_error("Both --major and --minor required to identify devices.");
		return EINVALID_CMD_LINE;
	}
	
	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_READ, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	/*
	 * This a special case where use_lvmetad=1 in lvm.conf but pvscan
	 * cannot use lvmetad for some reason.  In this case pvscan should
	 * still activate LVs even though it's not updating the cache.
	 */
	if (do_activate && !lvmetad_used()) {
		log_verbose("Activating all VGs without lvmetad.");
		all_vgs = 1;
		devno_args = 0;
		goto activate;
	}

	/*
	 * Scan all devices when no args are given.
	 */
	if (!argc && !devno_args) {
		log_verbose("Scanning all devices.");

		if (!lvmetad_pvscan_all_devs(cmd, 1)) {
			log_warn("WARNING: Not using lvmetad because cache update failed.");
			lvmetad_make_unused(cmd);
		}
		if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
			log_warn("WARNING: Not using lvmetad because %s.", reason);
			lvmetad_make_unused(cmd);
		}
		all_vgs = 1;
		goto activate;
	}
       
	/*
	 * FIXME: when specific devs are named, we generally don't want to scan
	 * any other devs, but if lvmetad is not yet populated, the first
	 * 'pvscan --cache dev' does need to do a full scan.  We want to remove
	 * the need for this case so that 'pvscan --cache dev' is guaranteed to
	 * never scan any devices other than those specified.
	 */
	if (!lvmetad_token_matches(cmd)) {
		log_verbose("Scanning all devices to initialize lvmetad.");

		if (lvmetad_used() && !lvmetad_pvscan_all_devs(cmd, 0)) {
			log_warn("WARNING: Not using lvmetad because cache update failed.");
			lvmetad_make_unused(cmd);
		}
		if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
			log_warn("WARNING: Not using lvmetad because %s.", reason);
			lvmetad_make_unused(cmd);
		}
		all_vgs = 1;
		goto activate;
	}

	/*
	 * When args are given, scan only those devices.  If lvmetad is already
	 * disabled, a full scan is required to reenable it, so there's no
	 * point in doing individual device scans, so go directly to
	 * autoactivation.  (FIXME: Should we also skip autoactivation in this
	 * case since that will read disks with lvmetad disabled?
	 * i.e. avoid disk access and not activate LVs, or or read from disk
	 * and activate LVs?)
	 */
	if (lvmetad_is_disabled(cmd, &reason)) {
		log_warn("WARNING: Not using lvmetad because %s.", reason);
		lvmetad_make_unused(cmd);
		all_vgs = 1;
		goto activate;
	}

	/*
	 * Step 1: for each device, if it's no longer found, then tell lvmetad
	 * to drop it.  If the device exists, read metadata from it and send
	 * that to lvmetad.
	 *
	 * When given a device name, check if the device is not visible to
	 * lvmetad, but still visible to the system, and if so, tell lvmetad to
	 * drop it (using the major:minor from the system).
	 *
	 * When given a major:minor which is not visible to the system, just
	 * tell lvmetad to drop it directly using that major:minor.
	 *
	 * When a device has left the system, it must be dropped using
	 * --major/--minor because we cannot map the device name to major:minor
	 *  after the device has left.  (A full rescan could of course be used
	 *  to drop any devices that have left.)
	 */

	if (argc || devno_args) {
		log_verbose("Scanning devices on command line.");
		cmd->pvscan_cache_single = 1;
	}

	/* Creates a list of dev names from /dev, sysfs, etc; does not read any. */
	dev_cache_scan();

	dm_list_init(&single_devs);

	while (argc--) {
		pv_name = *argv++;
		if (pv_name[0] == '/') {
			if (!(dev = dev_cache_get(cmd, pv_name, cmd->lvmetad_filter))) {
				/* Remove device path from lvmetad. */
				log_debug("Removing dev %s from lvmetad cache.", pv_name);
				if ((dev = dev_cache_get(cmd, pv_name, NULL))) {
					if (!_lvmetad_clear_dev(dev->dev, MAJOR(dev->dev), MINOR(dev->dev)))
						remove_errors++;
				} else {
					log_error("Physical Volume %s not found.", pv_name);
					ret = ECMD_FAILED;
				}
			} else {
				/*
				 * Scan device.  This dev could still be
				 * removed from lvmetad below if it doesn't
				 * pass other filters.
				 */
				log_debug("Scanning dev %s for lvmetad cache.", pv_name);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&single_devs, &devl->list);
			}
		} else {
			if (sscanf(pv_name, "%d:%d", &major, &minor) != 2) {
				log_warn("WARNING: Failed to parse major:minor from %s, skipping.", pv_name);
				continue;
			}
			devno = MKDEV((dev_t)major, (dev_t)minor);

			if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->lvmetad_filter))) {
				/* Remove major:minor from lvmetad. */
				log_debug("Removing dev %d:%d from lvmetad cache.", major, minor);
				if (!_lvmetad_clear_dev(devno, major, minor))
					remove_errors++;
			} else {
				/*
				 * Scan device.  This dev could still be
				 * removed from lvmetad below if it doesn't
				 * pass other filters.
				 */
				log_debug("Scanning dev %d:%d for lvmetad cache.", major, minor);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&single_devs, &devl->list);
			}
		}

		if (sigint_caught()) {
			ret = ECMD_FAILED;
			goto_out;
		}
	}

	if (!dm_list_empty(&single_devs)) {
		label_scan_devs(cmd, cmd->lvmetad_filter, &single_devs);

		dm_list_iterate_items(devl, &single_devs) {
			dev = devl->dev;

			if (dev->flags & DEV_FILTER_OUT_SCAN) {
				log_debug("Removing dev %s from lvmetad cache after scan.", dev_name(dev));
				if (!_lvmetad_clear_dev(dev->dev, MAJOR(dev->dev), MINOR(dev->dev)))
					remove_errors++;
				continue;
			}

			/*
			 * Devices that exist and pass the lvmetad filter
			 * are added to lvmetad.
			 */
			if (!lvmetad_pvscan_single(cmd, dev, &found_vgnames, &pp.changed_vgnames))
				add_errors++;
		}
	}

	if (!devno_args)
		goto activate;

	dm_list_init(&single_devs);

	/* Process any grouped --major --minor args */
	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		major = grouped_arg_int_value(current_group->arg_values, major_ARG, major);
		minor = grouped_arg_int_value(current_group->arg_values, minor_ARG, minor);

		if (major < 0 || minor < 0)
			continue;

		devno = MKDEV((dev_t)major, (dev_t)minor);

		if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->lvmetad_filter))) {
			/* Remove major:minor from lvmetad. */
			log_debug("Removing dev %d:%d from lvmetad cache.", major, minor);
			if (!_lvmetad_clear_dev(devno, major, minor))
				remove_errors++;
		} else {
			/* Add major:minor to lvmetad. */
			log_debug("Scanning dev %d:%d for lvmetad cache.", major, minor);

			if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
				return_0;
			devl->dev = dev;
			dm_list_add(&single_devs, &devl->list);
		}

		if (sigint_caught()) {
			ret = ECMD_FAILED;
			goto_out;
		}
	}

	if (!dm_list_empty(&single_devs)) {
		label_scan_devs(cmd, cmd->lvmetad_filter, &single_devs);

		dm_list_iterate_items(devl, &single_devs) {
			dev = devl->dev;

			if (dev->flags & DEV_FILTER_OUT_SCAN) {
				log_debug("Removing dev %s from lvmetad cache after scan.", dev_name(dev));
				if (!_lvmetad_clear_dev(dev->dev, MAJOR(dev->dev), MINOR(dev->dev)))
					remove_errors++;
				continue;
			}

			/*
			 * Devices that exist and pass the lvmetad filter
			 * are added to lvmetad.
			 */
			if (!lvmetad_pvscan_single(cmd, devl->dev, &found_vgnames, &pp.changed_vgnames))
				add_errors++;
		}
	}

	/*
	 * In the process of scanning devices, lvmetad may have become
	 * disabled.  If so, revert to scanning for the autoactivation step.
	 * Only autoactivate the VGs that were found during the dev scans.
	 */
	if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
		log_warn("WARNING: Not using lvmetad because %s.", reason);
		lvmetad_make_unused(cmd);
	}

activate:
	/*
	 * Step 2: when the PV was sent to lvmetad, the lvmetad reply
	 * indicated if all the PVs for the VG are now found.  If so,
	 * the vgname was added to the list, and we can attempt to
	 * autoactivate LVs in the VG.
	 */
	if (do_activate)
		ret = _pvscan_autoactivate(cmd, &pp, all_vgs, &found_vgnames);

out:
	if (remove_errors || add_errors || pp.activate_errors)
		ret = ECMD_FAILED;

	if (!sync_local_dev_names(cmd))
		stack;
	unlock_vg(cmd, NULL, VG_GLOBAL);
	return ret;
}

/*
 * Three main pvscan cases related to lvmetad usage:
 * 1. pvscan
 * 2. pvscan --cache
 * 3. pvscan --cache <dev>
 *
 * 1. The 'pvscan' command (without --cache) may or may not attempt to
 * repopulate the lvmetad cache, and may or may not use the lvmetad
 * cache to display PV info:
 *
 * i. If lvmetad is being used and is in a normal state, then 'pvscan'
 * will simply read and display PV info from the lvmetad cache.
 *
 * ii. If lvmetad is not being used, 'pvscan' will read all devices to
 * display the PV info.
 *
 * iii. If lvmetad is being used, but has been disabled (because of
 * duplicate devs), or has a non-matching token
 * (because the device filter is different from the device filter last
 * used to populate lvmetad), then 'pvscan' will begin by rescanning
 * devices to repopulate lvmetad.  If lvmetad is enabled after the
 * rescan, then 'pvscan' will simply read and display PV info from the
 * lvmetad cache (like case i).  If lvmetad is disabled after the
 * rescan, then 'pvscan' will read all devices to display PV info
 * (like case ii).
 *
 * 2. The 'pvscan --cache' command (without named devs) will always
 * attempt to repopulate the lvmetad cache by rescanning all devs
 * (regardless of whether lvmetad was previously disabled or had an
 * unmatching token.)  lvmetad may be enabled or disabled after the
 * rescan (depending on whether duplicate devs).
 *
 * 3. The 'pvscan --cache <dev>' command will attempt to repopulate the
 * lvmetad cache by rescanning all devs if lvmetad has a non-matching
 * token (e.g. because it has not yet been populated, see FIXME above).
 * Otherwise, the command will only rescan the named <dev> and send
 * their metadata to lvmetad.
 */

int pvscan(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_params params = { 0 };
	struct processing_handle *handle = NULL;
	const char *reason = NULL;
	int ret;

	if (arg_is_set(cmd, cache_long_ARG) && !lvmetad_used())
		return _pvscan_online_aa(cmd, argc, argv);

	if (arg_is_set(cmd, cache_long_ARG))
		return _pvscan_cache(cmd, argc, argv);

	if (argc) {
		log_error("Too many parameters on command line.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, activate_ARG)) {
		log_error("--activate is only valid with --cache.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, major_ARG) || arg_is_set(cmd, minor_ARG)) {
		log_error("--major and --minor are only valid with --cache.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, novolumegroup_ARG) && arg_is_set(cmd, exported_ARG)) {
		log_error("Options -e and -n are incompatible");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, exported_ARG) || arg_is_set(cmd, novolumegroup_ARG))
		log_warn("WARNING: only considering physical volumes %s",
			  arg_is_set(cmd, exported_ARG) ?
			  "of exported volume group(s)" : "in no volume group");

	/* Needed because this command has NO_LVMETAD_AUTOSCAN. */
	if (lvmetad_used() && (!lvmetad_token_matches(cmd) || lvmetad_is_disabled(cmd, &reason))) {
		if (lvmetad_used() && !lvmetad_pvscan_all_devs(cmd, 0)) {
			log_warn("WARNING: Not using lvmetad because cache update failed.");
			lvmetad_make_unused(cmd);
		}

		if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
			log_warn("WARNING: Not using lvmetad because %s.", reason);
			lvmetad_make_unused(cmd);
		}
	}

	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
	}

	handle->custom_handle = &params;

	ret = process_each_pv(cmd, argc, argv, NULL, 0, 0, handle, _pvscan_display_single);

	if (!params.pvs_found)
		log_print_unless_silent("No matching physical volumes found");
	else
		log_print_unless_silent("Total: %d [%s] / in use: %d [%s] / in no VG: %d [%s]",
					params.pvs_found,
					display_size(cmd, params.size_total),
					params.pvs_found - params.new_pvs_found,
					display_size(cmd, (params.size_total - params.size_new)),
					params.new_pvs_found, display_size(cmd, params.size_new));

out:
	unlock_vg(cmd, NULL, VG_GLOBAL);
	destroy_processing_handle(cmd, handle);

	return ret;
}

static const char *_pvs_online_dir = DEFAULT_RUN_DIR "/pvs_online";

/*
 * When a device goes offline we only know its major:minor, not its PVID.
 * Since the dev isn't around, we can't read it to get its PVID, so we have to
 * read the PVID files to find the one containing this major:minor and remove
 * that one. This means that the PVID files need to contain the devno's they
 * were created from.
 */

static void _online_pvid_file_remove_devno(int major, int minor)
{
	char path[PATH_MAX];
	char buf[32];
	char buf_in[32];
	DIR *dir;
	struct dirent *de;
	int fd, rv;

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d:%d\n", major, minor);

	log_debug("Remove pv online devno %d:%d", major, minor);

	if (!(dir = opendir(_pvs_online_dir)))
		return;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, de->d_name);

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			log_debug("Failed to open %s", path);
			continue;
		}

		rv = read(fd, buf_in, sizeof(buf_in));
		if (!rv || rv < 0) {
			log_debug("Failed to read %s", path);
			continue;
		}

		if (!strncmp(buf, buf_in, strlen(buf))) {
			log_debug("Unlink pv online %s %s", buf, path);
			unlink(path);
			break;
		}
	}
	closedir(dir);
}

static void _online_pvid_files_remove(void)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *de;

	if (!(dir = opendir(_pvs_online_dir)))
		return;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, de->d_name);
		unlink(path);
	}
	closedir(dir);
}

static void _online_pvid_file_create(struct device *dev)
{
	char path[PATH_MAX];
	char buf[32];
	int major, minor;
	int fd;
	int rv;

	memset(path, 0, sizeof(path));

	major = (int)MAJOR(dev->dev);
	minor = (int)MINOR(dev->dev);

	snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, dev->pvid);

	snprintf(buf, sizeof(buf), "%d:%d\n", major, minor);

	log_debug("Create pv online: %s %d:%d %s", path, major, minor, dev_name(dev));

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		log_warn("Failed to open %s: %d", path, errno);
		return;
	}

	rv = write(fd, buf, strlen(buf));
	if (!rv || rv < 0)
		log_warn("Failed to write fd %d buf %s dev %s to %s: %d",
			 fd, buf, dev_name(dev), path, errno);

	/* We don't care about syncing, these files are not even persistent. */

	close(fd);
}

static int _online_pvid_file_exists(const char *pvid)
{
	char path[PATH_MAX];
	struct stat buf;
	int rv;

	memset(path, 0, sizeof(path));

	snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, pvid);

	log_debug("Check pv online: %s", path);

	rv = stat(path, &buf);
	if (!rv) {
		log_debug("Check pv online: yes");
		return 1;
	}
	log_debug("Check pv online: no");
	return 0;
}

static void _online_pvid_dir_setup(void)
{
	struct stat st;
	int rv;

	if (!stat(_pvs_online_dir, &st))
		return;

	dm_prepare_selinux_context(_pvs_online_dir, S_IFDIR);
	rv = mkdir(_pvs_online_dir, 0777);
	dm_prepare_selinux_context(NULL, 0);

	if (rv < 0)
		log_debug("Failed to create %s", _pvs_online_dir);
}

static int _online_pvid_files_missing(void)
{
	DIR *dir;
	struct dirent *de;

	if (!(dir = opendir(_pvs_online_dir))) {
		log_debug("Failed to open %s", _pvs_online_dir);
		return 1;
	}

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;
		closedir(dir);
		return 0;
	}
	closedir(dir);
	return 1;
}

static int _online_pv_found(struct cmd_context *cmd,
			    struct device *dev, struct dm_list *dev_args,
			    struct volume_group *vg,
			    struct dm_list *found_vgnames)
{
	struct pv_list *pvl;
	int pvids_not_online = 0;
	int dev_args_in_vg = 0;

	/*
	 * Create file named for pvid to record this PV is online.
	 */

	_online_pvid_file_create(dev);

	if (!vg || !found_vgnames)
		return 1;

	/*
	 * Check if all the PVs for this VG are online.  This is only
	 * needed when autoactivating the VG which should be run only
	 * when the VG is complete.  If the arrival of this dev completes
	 * the VG, then we want to activate the VG.
	 */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!_online_pvid_file_exists((const char *)&pvl->pv->id.uuid))
			pvids_not_online++;

		/* Check if one of the devs on the command line is in this VG. */
		if (dev_args && dev_in_device_list(pvl->pv->dev, dev_args))
			dev_args_in_vg = 1;
	}

	/*
	 * Return if we did not find an online file for one of the PVIDs
	 * in the VG, which means the VG is not yet complete.
	 */

	if (pvids_not_online)
		return 1;

	/*
	 * When all PVIDs from the VG are online, then add vgname to
	 * found_vgnames.
	 */

	log_debug("online dev %s completes VG %s.", dev_name(dev), vg->name);

	/*
	 * We either want to return all complete VGs that are found on any devs
	 * we are scanning, or we want to return complete VGs only when they
	 * contain PVs that were specified on the command line.
	 */

	if (!dev_args || dev_args_in_vg) {
		log_debug("online dev %s can autoactivate VG %s", dev_name(dev), vg->name);
		if (!str_list_add(cmd->mem, found_vgnames, dm_pool_strdup(cmd->mem, vg->name)))
			stack;
	}

	return 1;
}

struct _pvscan_baton {
	struct cmd_context *cmd;
	struct volume_group *vg;
	struct format_instance *fid;
};

static int _online_pvscan_single(struct metadata_area *mda, void *baton)
{
	struct _pvscan_baton *b = baton;
	struct volume_group *vg;

	if (mda_is_ignored(mda) ||
	    !(vg = mda->ops->vg_read(b->fid, "", mda, NULL, NULL)))
		return 1;

	/* FIXME Also ensure contents match etc. */
	if (!b->vg || vg->seqno > b->vg->seqno)
		b->vg = vg;
	else if (b->vg)
		release_vg(vg);

	return 1;
}

/*
 * disable_remove is 1 when resetting the online state, which begins with
 * removing all pvid files, and then creating new pvid files for PVs that
 * are found, so we don't need to try to remove pvid files here when a PV
 * is not found on a device.
 */

static int _online_pvscan_one(struct cmd_context *cmd, struct device *dev,
			      struct dm_list *dev_args,
			      struct dm_list *found_vgnames,
			      int disable_remove,
			      const char **pvid_without_metadata)
{
	struct label *label;
	struct lvmcache_info *info;
	struct _pvscan_baton baton;
	const struct format_type *fmt;
	/* Create a dummy instance. */
	struct format_instance_ctx fic = { .type = 0 };
	int ret = 0;

	log_debug("pvscan metadata from dev %s", dev_name(dev));

	if (udev_dev_is_mpath_component(dev)) {
		log_debug("Ignore multipath component for pvscan.");
		return 1;
	}

	if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
		log_debug("No PV info found on %s for PVID %s.", dev_name(dev), dev->pvid);
		if (!disable_remove)
			_online_pvid_file_remove_devno((int)MAJOR(dev->dev), (int)MINOR(dev->dev));
		return 1;
	}

	if (!(label = lvmcache_get_label(info))) {
		log_debug("No PV label found for %s.", dev_name(dev));
		if (!disable_remove)
			_online_pvid_file_remove_devno((int)MAJOR(dev->dev), (int)MINOR(dev->dev));
		return 1;
	}

	fmt = lvmcache_fmt(info);

	baton.cmd = cmd;
	baton.vg = NULL;
	baton.fid = fmt->ops->create_instance(fmt, &fic);

	if (!baton.fid) {
		ret = 0;
		goto_out;
	}

	lvmcache_foreach_mda(info, _online_pvscan_single, &baton);

	if (!baton.vg) {
		if (pvid_without_metadata)
			*pvid_without_metadata = dm_pool_strdup(cmd->mem, dev->pvid);
		fmt->ops->destroy_instance(baton.fid);
	}

	ret = _online_pv_found(cmd, dev, dev_args, baton.vg, found_vgnames);

	release_vg(baton.vg);
out:
	return ret;
}

/*
 * dev_args is the list of devices that were specified on the
 * pvscan command line.
 *
 * . When dev_args is NULL, any complete VGs that are found will
 *   be returned in found_vgnames.
 *
 * . When dev_args is set, then complete VGs that that contain
 *   devs in dev_args will be returned in found_vgnames.
 */

static void _online_pvscan_all_devs(struct cmd_context *cmd,
				    struct dm_list *found_vgnames,
				    struct dm_list *dev_args)
{
	struct dev_iter *iter;
	struct device *dev;

	label_scan(cmd);

	if (!(iter = dev_iter_create(cmd->lvmetad_filter, 1))) {
		log_error("dev_iter creation failed");
		return;
	}

	while ((dev = dev_iter_get(cmd, iter))) {
		if (sigint_caught()) {
			stack;
			break;
		}

		if (!_online_pvscan_one(cmd, dev, dev_args, found_vgnames, 1, NULL)) {
			stack;
			break;
		}
	}

	dev_iter_destroy(iter);
}

static int _pvscan_aa_single(struct cmd_context *cmd, const char *vg_name,
			     struct volume_group *vg, struct processing_handle *handle)
{
	struct pvscan_aa_params *pp = (struct pvscan_aa_params *)handle->custom_handle;

	if (vg_is_clustered(vg))
		return ECMD_PROCESSED;

	if (vg_is_exported(vg))
		return ECMD_PROCESSED;

	if (vg_is_shared(vg))
		return ECMD_PROCESSED;

	log_debug("pvscan autoactivating VG %s.", vg_name);

	if (!vgchange_activate(cmd, vg, CHANGE_AAY)) {
		log_error("%s: autoactivation failed.", vg->name);
		pp->activate_errors++;
	}

	return ECMD_PROCESSED;
}

static int _pvscan_aa(struct cmd_context *cmd, struct pvscan_aa_params *pp,
		      int all_vgs, struct dm_list *vgnames)
{
	struct processing_handle *handle = NULL;
	int ret;

	if (!all_vgs && dm_list_empty(vgnames)) {
		log_debug("No VGs to autoactivate.");
		return ECMD_PROCESSED;
	}

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = pp;

	if (all_vgs) {
		cmd->cname->flags |= ALL_VGS_IS_DEFAULT;
		pp->refresh_all = 1;
	}

	ret = process_each_vg(cmd, 0, NULL, NULL, vgnames, READ_FOR_UPDATE, 0, handle, _pvscan_aa_single);

	destroy_processing_handle(cmd, handle);

	return ret;
}

static int _pvscan_online_aa(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_aa_params pp = { 0 };
	struct dm_list single_devs;
	struct dm_list found_vgnames;
	struct device *dev;
	struct device_list *devl;
	const char *pv_name;
	const char *pvid_without_metadata = NULL;
	int32_t major = -1;
	int32_t minor = -1;
	int devno_args = 0;
	struct arg_value_group_list *current_group;
	dev_t devno;
	int do_activate = arg_is_set(cmd, activate_ARG);
	int all_vgs = 0;
	int add_errors = 0;
	int ret = ECMD_PROCESSED;

	dm_list_init(&found_vgnames);

	if (arg_is_set(cmd, major_ARG) + arg_is_set(cmd, minor_ARG))
		devno_args = 1;

	if (devno_args && (!arg_is_set(cmd, major_ARG) || !arg_is_set(cmd, minor_ARG))) {
		log_error("Both --major and --minor required to identify devices.");
		return EINVALID_CMD_LINE;
	}

	_online_pvid_dir_setup();
	
	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_READ, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	/*
	 * Scan all devices when no args are given.
	 */
	if (!argc && !devno_args) {
		log_verbose("pvscan all devices.");
		_online_pvid_files_remove();
		_online_pvscan_all_devs(cmd, NULL, NULL);
		all_vgs = 1;
		goto activate;
	}

	if (_online_pvid_files_missing()) {
		log_verbose("pvscan all devices to initialize available PVs.");
		_online_pvid_files_remove();
		_online_pvscan_all_devs(cmd, NULL, NULL);
		all_vgs = 1;
		goto activate;
	}

	if (argc || devno_args) {
		log_verbose("pvscan devices on command line.");
		cmd->pvscan_cache_single = 1;
	}

	/* Creates a list of dev names from /dev, sysfs, etc; does not read any. */
	dev_cache_scan();

	dm_list_init(&single_devs);

	while (argc--) {
		pv_name = *argv++;
		if (pv_name[0] == '/') {
			if (!(dev = dev_cache_get(cmd, pv_name, cmd->lvmetad_filter))) {
				log_debug("pvscan arg %s not found.", pv_name);
				if ((dev = dev_cache_get(cmd, pv_name, NULL))) {
					/* nothing to do for this dev name */
				} else {
					log_error("Physical Volume %s not found.", pv_name);
					ret = ECMD_FAILED;
				}
			} else {
				/*
				 * Scan device.  This dev could still be removed
				 * below if it doesn't pass other filters.
				 */
				log_debug("pvscan arg %s found.", pv_name);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&single_devs, &devl->list);
			}
		} else {
			if (sscanf(pv_name, "%d:%d", &major, &minor) != 2) {
				log_warn("WARNING: Failed to parse major:minor from %s, skipping.", pv_name);
				continue;
			}
			devno = MKDEV((dev_t)major, (dev_t)minor);

			if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->lvmetad_filter))) {
				log_debug("pvscan arg %d:%d not found.", major, minor);
				_online_pvid_file_remove_devno(major, minor);
			} else {
				/*
				 * Scan device.  This dev could still be removed
				 * below if it doesn't pass other filters.
				 */
				log_debug("pvscan arg %d:%d found.", major, minor);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&single_devs, &devl->list);
			}
		}

		if (sigint_caught()) {
			ret = ECMD_FAILED;
			goto_out;
		}
	}

	if (!dm_list_empty(&single_devs)) {
		label_scan_devs(cmd, cmd->lvmetad_filter, &single_devs);

		dm_list_iterate_items(devl, &single_devs) {
			dev = devl->dev;

			if (dev->flags & DEV_FILTER_OUT_SCAN)
				continue;

			/*
			 * Devices that exist and pass the lvmetad filter
			 * are online.
			 */
			if (!_online_pvscan_one(cmd, dev, NULL, &found_vgnames, 0, &pvid_without_metadata))
				add_errors++;
		}
	}

	if (!devno_args)
		goto activate;

	dm_list_init(&single_devs);

	/* Process any grouped --major --minor args */
	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		major = grouped_arg_int_value(current_group->arg_values, major_ARG, major);
		minor = grouped_arg_int_value(current_group->arg_values, minor_ARG, minor);

		if (major < 0 || minor < 0)
			continue;

		devno = MKDEV((dev_t)major, (dev_t)minor);

		if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->lvmetad_filter))) {
			log_debug("pvscan arg %d:%d not found.", major, minor);
			_online_pvid_file_remove_devno(major, minor);
		} else {
			log_debug("pvscan arg %d:%d found.", major, minor);

			if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
				return_0;
			devl->dev = dev;
			dm_list_add(&single_devs, &devl->list);
		}

		if (sigint_caught()) {
			ret = ECMD_FAILED;
			goto_out;
		}
	}

	if (!dm_list_empty(&single_devs)) {
		label_scan_devs(cmd, cmd->lvmetad_filter, &single_devs);

		dm_list_iterate_items(devl, &single_devs) {
			dev = devl->dev;

			if (dev->flags & DEV_FILTER_OUT_SCAN)
				continue;

			/*
			 * Devices that exist and pass the lvmetad filter
			 * are online.
			 */
			if (!_online_pvscan_one(cmd, devl->dev, NULL, &found_vgnames, 0, &pvid_without_metadata))
				add_errors++;
		}
	}

activate:
	/*
	 * Special case: pvscan --cache -aay dev 
	 * where dev has no VG metadata, and it's the final device to
	 * complete the VG.  In this case we want to autoactivate the
	 * VG, but the scanned device does not know what VG it's in or
	 * whether that VG is now complete.  In this case we need to
	 * scan all devs and pick out the complete VG holding this
	 * device so we can then autoactivate that VG.
	 */
	if (!dm_list_empty(&single_devs) && dm_list_empty(&found_vgnames) &&
	    pvid_without_metadata && do_activate) {
		log_verbose("pvscan all devices for PV without metadata: %s.", pvid_without_metadata);
		_online_pvscan_all_devs(cmd, &found_vgnames, &single_devs);
	}

	/*
	 * Step 2: when the PV was recorded online, we check if all the
	 * PVs for the VG are online.  If so, the vgname was added to the
	 * list, and we can attempt to autoactivate LVs in the VG.
	 */
	if (do_activate)
		ret = _pvscan_aa(cmd, &pp, all_vgs, &found_vgnames);

out:
	if (add_errors || pp.activate_errors)
		ret = ECMD_FAILED;

	if (!sync_local_dev_names(cmd))
		stack;
	unlock_vg(cmd, NULL, VG_GLOBAL);
	return ret;
}

