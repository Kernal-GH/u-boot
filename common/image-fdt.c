/*
 * Copyright (c) 2013, Google Inc.
 *
 * (C) Copyright 2008 Semihalf
 *
 * (C) Copyright 2000-2006
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <common.h>
#include <fdt_support.h>
#include <errno.h>
#include <image.h>
#include <libfdt.h>
#include <asm/io.h>

#ifndef CONFIG_SYS_FDT_PAD
#define CONFIG_SYS_FDT_PAD 0x3000
#endif

DECLARE_GLOBAL_DATA_PTR;

static void fdt_error(const char *msg)
{
	puts("ERROR: ");
	puts(msg);
	puts(" - must RESET the board to recover.\n");
}

static const image_header_t *image_get_fdt(ulong fdt_addr)
{
	const image_header_t *fdt_hdr = map_sysmem(fdt_addr, 0);

	image_print_contents(fdt_hdr);

	puts("   Verifying Checksum ... ");
	if (!image_check_hcrc(fdt_hdr)) {
		fdt_error("fdt header checksum invalid");
		return NULL;
	}

	if (!image_check_dcrc(fdt_hdr)) {
		fdt_error("fdt checksum invalid");
		return NULL;
	}
	puts("OK\n");

	if (!image_check_type(fdt_hdr, IH_TYPE_FLATDT)) {
		fdt_error("uImage is not a fdt");
		return NULL;
	}
	if (image_get_comp(fdt_hdr) != IH_COMP_NONE) {
		fdt_error("uImage is compressed");
		return NULL;
	}
	if (fdt_check_header((char *)image_get_data(fdt_hdr)) != 0) {
		fdt_error("uImage data is not a fdt");
		return NULL;
	}
	return fdt_hdr;
}

/**
 * boot_fdt_add_mem_rsv_regions - Mark the memreserve sections as unusable
 * @lmb: pointer to lmb handle, will be used for memory mgmt
 * @fdt_blob: pointer to fdt blob base address
 *
 * Adds the memreserve regions in the dtb to the lmb block.  Adding the
 * memreserve regions prevents u-boot from using them to store the initrd
 * or the fdt blob.
 */
void boot_fdt_add_mem_rsv_regions(struct lmb *lmb, void *fdt_blob)
{
	uint64_t addr, size;
	int i, total;

	if (fdt_check_header(fdt_blob) != 0)
		return;

	total = fdt_num_mem_rsv(fdt_blob);
	for (i = 0; i < total; i++) {
		if (fdt_get_mem_rsv(fdt_blob, i, &addr, &size) != 0)
			continue;
		printf("   reserving fdt memory region: addr=%llx size=%llx\n",
		       (unsigned long long)addr, (unsigned long long)size);
		lmb_reserve(lmb, addr, size);
	}
}

/**
 * boot_relocate_fdt - relocate flat device tree
 * @lmb: pointer to lmb handle, will be used for memory mgmt
 * @of_flat_tree: pointer to a char* variable, will hold fdt start address
 * @of_size: pointer to a ulong variable, will hold fdt length
 *
 * boot_relocate_fdt() allocates a region of memory within the bootmap and
 * relocates the of_flat_tree into that region, even if the fdt is already in
 * the bootmap.  It also expands the size of the fdt by CONFIG_SYS_FDT_PAD
 * bytes.
 *
 * of_flat_tree and of_size are set to final (after relocation) values
 *
 * returns:
 *      0 - success
 *      1 - failure
 */
int boot_relocate_fdt(struct lmb *lmb, char **of_flat_tree, ulong *of_size)
{
	void	*fdt_blob = *of_flat_tree;
	void	*of_start = NULL;
	char	*fdt_high;
	ulong	of_len = 0;
	int	err;
	int	disable_relocation = 0;

	/* nothing to do */
	if (*of_size == 0)
		return 0;

	if (fdt_check_header(fdt_blob) != 0) {
		fdt_error("image is not a fdt");
		goto error;
	}

	/* position on a 4K boundary before the alloc_current */
	/* Pad the FDT by a specified amount */
	of_len = *of_size + CONFIG_SYS_FDT_PAD;

	/* If fdt_high is set use it to select the relocation address */
	fdt_high = getenv("fdt_high");
	if (fdt_high) {
		void *desired_addr = (void *)simple_strtoul(fdt_high, NULL, 16);

		if (((ulong) desired_addr) == ~0UL) {
			/* All ones means use fdt in place */
			of_start = fdt_blob;
			lmb_reserve(lmb, (ulong)of_start, of_len);
			disable_relocation = 1;
		} else if (desired_addr) {
			of_start =
			    (void *)(ulong) lmb_alloc_base(lmb, of_len, 0x1000,
							   (ulong)desired_addr);
			if (of_start == NULL) {
				puts("Failed using fdt_high value for Device Tree");
				goto error;
			}
		} else {
			of_start =
			    (void *)(ulong) lmb_alloc(lmb, of_len, 0x1000);
		}
	} else {
		of_start =
		    (void *)(ulong) lmb_alloc_base(lmb, of_len, 0x1000,
						   getenv_bootm_mapsize()
						   + getenv_bootm_low());
	}

	if (of_start == NULL) {
		puts("device tree - allocation error\n");
		goto error;
	}

	if (disable_relocation) {
		/*
		 * We assume there is space after the existing fdt to use
		 * for padding
		 */
		fdt_set_totalsize(of_start, of_len);
		printf("   Using Device Tree in place at %p, end %p\n",
		       of_start, of_start + of_len - 1);
	} else {
		debug("## device tree at %p ... %p (len=%ld [0x%lX])\n",
		      fdt_blob, fdt_blob + *of_size - 1, of_len, of_len);

		printf("   Loading Device Tree to %p, end %p ... ",
		       of_start, of_start + of_len - 1);

		err = fdt_open_into(fdt_blob, of_start, of_len);
		if (err != 0) {
			fdt_error("fdt move failed");
			goto error;
		}
		puts("OK\n");
	}

	*of_flat_tree = of_start;
	*of_size = of_len;

	set_working_fdt_addr(*of_flat_tree);
	return 0;

error:
	return 1;
}

#if defined(CONFIG_FIT)
/**
 * fit_check_fdt - verify FIT format FDT subimage
 * @fit_hdr: pointer to the FIT  header
 * fdt_noffset: FDT subimage node offset within FIT image
 * @verify: data CRC verification flag
 *
 * fit_check_fdt() verifies integrity of the FDT subimage and from
 * specified FIT image.
 *
 * returns:
 *     1, on success
 *     0, on failure
 */
static int fit_check_fdt(const void *fit, int fdt_noffset, int verify)
{
	fit_image_print(fit, fdt_noffset, "   ");

	if (verify) {
		puts("   Verifying Hash Integrity ... ");
		if (!fit_image_verify(fit, fdt_noffset)) {
			fdt_error("Bad Data Hash");
			return 0;
		}
		puts("OK\n");
	}

	if (!fit_image_check_type(fit, fdt_noffset, IH_TYPE_FLATDT)) {
		fdt_error("Not a FDT image");
		return 0;
	}

	if (!fit_image_check_comp(fit, fdt_noffset, IH_COMP_NONE)) {
		fdt_error("FDT image is compressed");
		return 0;
	}

	return 1;
}
#endif

/**
 * boot_get_fdt - main fdt handling routine
 * @argc: command argument count
 * @argv: command argument list
 * @images: pointer to the bootm images structure
 * @of_flat_tree: pointer to a char* variable, will hold fdt start address
 * @of_size: pointer to a ulong variable, will hold fdt length
 *
 * boot_get_fdt() is responsible for finding a valid flat device tree image.
 * Curently supported are the following ramdisk sources:
 *      - multicomponent kernel/ramdisk image,
 *      - commandline provided address of decicated ramdisk image.
 *
 * returns:
 *     0, if fdt image was found and valid, or skipped
 *     of_flat_tree and of_size are set to fdt start address and length if
 *     fdt image is found and valid
 *
 *     1, if fdt image is found but corrupted
 *     of_flat_tree and of_size are set to 0 if no fdt exists
 */
int boot_get_fdt(int flag, int argc, char * const argv[],
		bootm_headers_t *images, char **of_flat_tree, ulong *of_size)
{
	const image_header_t *fdt_hdr;
	ulong		fdt_addr;
	char		*fdt_blob = NULL;
	ulong		image_start, image_data, image_end;
	ulong		load_start, load_end;
	void		*buf;
#if defined(CONFIG_FIT)
	void		*fit_hdr;
	const char	*fit_uname_config = NULL;
	const char	*fit_uname_fdt = NULL;
	ulong		default_addr;
	int		cfg_noffset;
	int		fdt_noffset;
	const void	*data;
	size_t		size;
#endif

	*of_flat_tree = NULL;
	*of_size = 0;

	if (argc > 3 || genimg_has_config(images)) {
#if defined(CONFIG_FIT)
		if (argc > 3) {
			/*
			 * If the FDT blob comes from the FIT image and the
			 * FIT image address is omitted in the command line
			 * argument, try to use ramdisk or os FIT image
			 * address or default load address.
			 */
			if (images->fit_uname_rd)
				default_addr = (ulong)images->fit_hdr_rd;
			else if (images->fit_uname_os)
				default_addr = (ulong)images->fit_hdr_os;
			else
				default_addr = load_addr;

			if (fit_parse_conf(argv[3], default_addr,
					   &fdt_addr, &fit_uname_config)) {
				debug("*  fdt: config '%s' from image at 0x%08lx\n",
				      fit_uname_config, fdt_addr);
			} else if (fit_parse_subimage(argv[3], default_addr,
				   &fdt_addr, &fit_uname_fdt)) {
				debug("*  fdt: subimage '%s' from image at 0x%08lx\n",
				      fit_uname_fdt, fdt_addr);
			} else
#endif
			{
				fdt_addr = simple_strtoul(argv[3], NULL, 16);
				debug("*  fdt: cmdline image address = 0x%08lx\n",
				      fdt_addr);
			}
#if defined(CONFIG_FIT)
		} else {
			/* use FIT configuration provided in first bootm
			 * command argument
			 */
			fdt_addr = map_to_sysmem(images->fit_hdr_os);
			fit_uname_config = images->fit_uname_cfg;
			debug("*  fdt: using config '%s' from image at 0x%08lx\n",
			      fit_uname_config, fdt_addr);

			/*
			 * Check whether configuration has FDT blob defined,
			 * if not quit silently.
			 */
			fit_hdr = images->fit_hdr_os;
			cfg_noffset = fit_conf_get_node(fit_hdr,
					fit_uname_config);
			if (cfg_noffset < 0) {
				debug("*  fdt: no such config\n");
				return 0;
			}

			fdt_noffset = fit_conf_get_fdt_node(fit_hdr,
					cfg_noffset);
			if (fdt_noffset < 0) {
				debug("*  fdt: no fdt in config\n");
				return 0;
			}
		}
#endif

		debug("## Checking for 'FDT'/'FDT Image' at %08lx\n",
		      fdt_addr);

		/* copy from dataflash if needed */
		fdt_addr = genimg_get_image(fdt_addr);

		/*
		 * Check if there is an FDT image at the
		 * address provided in the second bootm argument
		 * check image type, for FIT images get a FIT node.
		 */
		buf = map_sysmem(fdt_addr, 0);
		switch (genimg_get_format(buf)) {
		case IMAGE_FORMAT_LEGACY:
			/* verify fdt_addr points to a valid image header */
			printf("## Flattened Device Tree from Legacy Image at %08lx\n",
			       fdt_addr);
			fdt_hdr = image_get_fdt(fdt_addr);
			if (!fdt_hdr)
				goto error;

			/*
			 * move image data to the load address,
			 * make sure we don't overwrite initial image
			 */
			image_start = (ulong)fdt_hdr;
			image_data = (ulong)image_get_data(fdt_hdr);
			image_end = image_get_image_end(fdt_hdr);

			load_start = image_get_load(fdt_hdr);
			load_end = load_start + image_get_data_size(fdt_hdr);

			if (load_start == image_start ||
			    load_start == image_data) {
				fdt_blob = (char *)image_data;
				break;
			}

			if ((load_start < image_end) &&
			    (load_end > image_start)) {
				fdt_error("fdt overwritten");
				goto error;
			}

			debug("   Loading FDT from 0x%08lx to 0x%08lx\n",
			      image_data, load_start);

			memmove((void *)load_start,
				(void *)image_data,
				image_get_data_size(fdt_hdr));

			fdt_blob = (char *)load_start;
			break;
		case IMAGE_FORMAT_FIT:
			/*
			 * This case will catch both: new uImage format
			 * (libfdt based) and raw FDT blob (also libfdt
			 * based).
			 */
#if defined(CONFIG_FIT)
			/* check FDT blob vs FIT blob */
			if (fit_check_format(buf)) {
				/*
				 * FIT image
				 */
				fit_hdr = buf;
				printf("## Flattened Device Tree from FIT Image at %08lx\n",
				       fdt_addr);

				if (!fit_uname_fdt) {
					/*
					 * no FDT blob image node unit name,
					 * try to get config node first. If
					 * config unit node name is NULL
					 * fit_conf_get_node() will try to
					 * find default config node
					 */
					cfg_noffset = fit_conf_get_node(fit_hdr,
							fit_uname_config);

					if (cfg_noffset < 0) {
						fdt_error("Could not find configuration node\n");
						goto error;
					}

					fit_uname_config = fdt_get_name(fit_hdr,
							cfg_noffset, NULL);
					printf("   Using '%s' configuration\n",
					       fit_uname_config);

					fdt_noffset = fit_conf_get_fdt_node(
							fit_hdr,
							cfg_noffset);
					fit_uname_fdt = fit_get_name(fit_hdr,
							fdt_noffset, NULL);
				} else {
					/*
					 * get FDT component image node
					 * offset
					 */
					fdt_noffset = fit_image_get_node(
								fit_hdr,
								fit_uname_fdt);
				}
				if (fdt_noffset < 0) {
					fdt_error("Could not find subimage node\n");
					goto error;
				}

				printf("   Trying '%s' FDT blob subimage\n",
				       fit_uname_fdt);

				if (!fit_check_fdt(fit_hdr, fdt_noffset,
						   images->verify))
					goto error;

				/* get ramdisk image data address and length */
				if (fit_image_get_data(fit_hdr, fdt_noffset,
						       &data, &size)) {
					fdt_error("Could not find FDT subimage data");
					goto error;
				}

				/*
				 * verify that image data is a proper FDT
				 * blob
				 */
				if (fdt_check_header((char *)data) != 0) {
					fdt_error("Subimage data is not a FTD");
					goto error;
				}

				/*
				 * move image data to the load address,
				 * make sure we don't overwrite initial image
				 */
				image_start = (ulong)fit_hdr;
				image_end = fit_get_end(fit_hdr);

				if (fit_image_get_load(fit_hdr, fdt_noffset,
						       &load_start) == 0) {
					load_end = load_start + size;

					if ((load_start < image_end) &&
					    (load_end > image_start)) {
						fdt_error("FDT overwritten");
						goto error;
					}

					printf("   Loading FDT from 0x%08lx to 0x%08lx\n",
					       (ulong)data, load_start);

					memmove((void *)load_start,
						(void *)data, size);

					fdt_blob = (char *)load_start;
				} else {
					fdt_blob = (char *)data;
				}

				images->fit_hdr_fdt = fit_hdr;
				images->fit_uname_fdt = fit_uname_fdt;
				images->fit_noffset_fdt = fdt_noffset;
				break;
			} else
#endif
			{
				/*
				 * FDT blob
				 */
				fdt_blob = buf;
				debug("*  fdt: raw FDT blob\n");
				printf("## Flattened Device Tree blob at %08lx\n",
				       (long)fdt_addr);
			}
			break;
		default:
			puts("ERROR: Did not find a cmdline Flattened Device Tree\n");
			goto error;
		}

		printf("   Booting using the fdt blob at 0x%p\n", fdt_blob);

	} else if (images->legacy_hdr_valid &&
			image_check_type(&images->legacy_hdr_os_copy,
					 IH_TYPE_MULTI)) {
		ulong fdt_data, fdt_len;

		/*
		 * Now check if we have a legacy multi-component image,
		 * get second entry data start address and len.
		 */
		printf("## Flattened Device Tree from multi component Image at %08lX\n",
		       (ulong)images->legacy_hdr_os);

		image_multi_getimg(images->legacy_hdr_os, 2, &fdt_data,
				   &fdt_len);
		if (fdt_len) {
			fdt_blob = (char *)fdt_data;
			printf("   Booting using the fdt at 0x%p\n", fdt_blob);

			if (fdt_check_header(fdt_blob) != 0) {
				fdt_error("image is not a fdt");
				goto error;
			}

			if (fdt_totalsize(fdt_blob) != fdt_len) {
				fdt_error("fdt size != image size");
				goto error;
			}
		} else {
			debug("## No Flattened Device Tree\n");
			return 0;
		}
	} else {
		debug("## No Flattened Device Tree\n");
		return 0;
	}

	*of_flat_tree = fdt_blob;
	*of_size = fdt_totalsize(fdt_blob);
	debug("   of_flat_tree at 0x%08lx size 0x%08lx\n",
	      (ulong)*of_flat_tree, *of_size);

	return 0;

error:
	*of_flat_tree = NULL;
	*of_size = 0;
	return 1;
}

/*
 * Verify the device tree.
 *
 * This function is called after all device tree fix-ups have been enacted,
 * so that the final device tree can be verified.  The definition of "verified"
 * is up to the specific implementation.  However, it generally means that the
 * addresses of some of the devices in the device tree are compared with the
 * actual addresses at which U-Boot has placed them.
 *
 * Returns 1 on success, 0 on failure.  If 0 is returned, U-boot will halt the
 * boot process.
 */
__weak int ft_verify_fdt(void *fdt)
{
	return 1;
}

__weak int arch_fixup_memory_node(void *blob)
{
	return 0;
}

int image_setup_libfdt(bootm_headers_t *images, void *blob,
		       int of_size, struct lmb *lmb)
{
	ulong *initrd_start = &images->initrd_start;
	ulong *initrd_end = &images->initrd_end;
	int ret;

	if (fdt_chosen(blob, 1) < 0) {
		puts("ERROR: /chosen node create failed");
		puts(" - must RESET the board to recover.\n");
		return -1;
	}
	arch_fixup_memory_node(blob);
	if (IMAAGE_OF_BOARD_SETUP)
		ft_board_setup(blob, gd->bd);
	fdt_fixup_ethernet(blob);

	/* Delete the old LMB reservation */
	lmb_free(lmb, (phys_addr_t)(u32)(uintptr_t)blob,
		 (phys_size_t)fdt_totalsize(blob));

	ret = fdt_resize(blob);
	if (ret < 0)
		return ret;
	of_size = ret;

	if (*initrd_start && *initrd_end) {
		of_size += FDT_RAMDISK_OVERHEAD;
		fdt_set_totalsize(blob, of_size);
	}
	/* Create a new LMB reservation */
	lmb_reserve(lmb, (ulong)blob, of_size);

	fdt_initrd(blob, *initrd_start, *initrd_end, 1);
	if (!ft_verify_fdt(blob))
		return -1;

	return 0;
}
