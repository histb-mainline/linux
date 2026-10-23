// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hisilicon NAND Flash Controller V610 driver
 *
 * Copyright (c) 2013-2017 HiSilicon Technologies Co., Ltd.
 *
 * Support for the asynchronous, hardware-ECC path this block is used in --
 * read, program and erase. Two shapes come from the vendor's own programming
 * model and are load-bearing here: the program address drops the column, so a
 * frame is always written whole, and completion is polled, because the boot
 * software leaves the interrupt enable at zero.
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/of.h>
#include <linux/reset.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define HINFC610_MAX_CHIP			(1)
#define HINFC610_CHIP_DELAY			(25)
#define HINFC610_TIMEOUT_WAIT_US		(10)
#define HINFC610_TIMEOUT_US			(2000 * 1000)
#define HINFC610_ERASE_TIMEOUT_US		(10000 * 1000)

/* Bytes the controller leaves in its window for a read-id or a status read. */
#define HINFC610_NANDINFO_LEN			0x10

#define HINFC610_CON				0x00
#define HINFC610_CON_OP_MODE_NORMAL		BIT(0)
#define HINFC610_CON_PAGESIZE_MASK		GENMASK(3, 1)
#define HINFC610_CON_PAGESIZE_2K		(1)
#define HINFC610_CON_READY_BUSY_SEL		BIT(8)
#define HINFC610_CON_ECCTYPE_MASK		GENMASK(12, 9)
/* Set in the word the boot software leaves; named in neither vendor tree. */
#define HINFC610_CON_BIT7				BIT(7)
#define HINFC610_CON_ECCTYPE_8BIT_1K		(1)

/*
 * Read latch, write latch and the hold cycles between directions: five, seven
 * and three for this part, which is also the value the block is found with
 * (measured: PWIDTH = 0x375 under the vendor system).
 */
#define HINFC610_PWIDTH				0x04
#define HINFC610_PWIDTH_S(w, r, h)		((w) | ((r) << 4) | ((h) << 8))
#define HINFC610_PWIDTH_S40			HINFC610_PWIDTH_S(5, 7, 3)

#define HINFC610_CMD				0x0c
#define HINFC610_CMD_SEQ(cmd1, cmd2)		((cmd1) | ((cmd2) << 8))
#define HINFC610_CMD_SEQ3(cmd1, cmd2, cmd3)	((cmd1) | ((cmd2) << 8) | \
						 ((cmd3) << 16))

/* What a program costs the bus: enter the frame, program it, read status. */
#define HINFC610_CMD_PROGRAM			HINFC610_CMD_SEQ3(NAND_CMD_SEQIN, \
						 NAND_CMD_PAGEPROG, \
						 NAND_CMD_STATUS)
#define HINFC610_ADDRL				0x10
#define HINFC610_ADDRH				0x14
#define HINFC610_DATA_NUM			0x18
#define HINFC610_DATA_NUM_LEN_MASK			GENMASK(13, 0)
#define HINFC610_DATA_NUM_WINDOW_EN			BIT(14)

#define HINFC610_OP				0x1c
#define HINFC610_OP_READ_DATA_EN		BIT(1)
#define HINFC610_OP_WAIT_READY_EN		BIT(2)
#define HINFC610_OP_CMD2_EN			BIT(3)
#define HINFC610_OP_ADDR_EN			BIT(5)
#define HINFC610_OP_CMD1_EN			BIT(6)
#define HINFC610_OP_NF_CS_MASK			GENMASK(8, 7)
#define HINFC610_OP_NF_CS(cs)			FIELD_PREP(HINFC610_OP_NF_CS_MASK, (cs))
#define HINFC610_OP_ADDR_CYCLE_MASK		GENMASK(11, 9)
#define HINFC610_OP_ADDR_CYCLE(cycles)		FIELD_PREP(HINFC610_OP_ADDR_CYCLE_MASK, \
							   (cycles))

#define HINFC610_STATUS				0x20
#define HINFC610_STATUS_READY			BIT(0)

#define HINFC610_INTS				0x28
#define HINFC610_INTS_CE			BIT(5)
#define HINFC610_INTS_UE			BIT(6)
#define HINFC610_INTS_DMA_DONE			BIT(9)
#define HINFC610_INTCLR				0x2c
#define HINFC610_INTCLR_ALL			(HINFC610_INTS_CE | HINFC610_INTS_UE | \
						 HINFC610_INTS_DMA_DONE)

/*
 * Corrected-bit counts of the last ECC-protected read, one byte per step from
 * the lowest address up. Measured: 0x00000000 after a clean read.
 */
#define HINFC610_ECC_STAT			0xa0

#define HINFC610_DMA_CTRL			0x60
#define HINFC610_DMA_CTRL_START			BIT(0)
#define HINFC610_DMA_CTRL_WE			BIT(1)
#define HINFC610_DMA_CTRL_BURST			GENMASK(6, 4)
#define HINFC610_DMA_CTRL_ADDR_NUM		BIT(7)
#define HINFC610_DMA_CTRL_CS			GENMASK(9, 8)
#define HINFC610_DMA_ADDR_DATA			0x64
#define HINFC610_DMA_ADDR_OOB			0x68
#define HINFC610_DMA_PARA			0x70
#define HINFC610_DMA_PARA_READ			GENMASK(3, 0)

#define HINFC610_VERSION			0x74
#define HINFC610_VERSION_610			0x610

/* Spare-area map as the raw window read reports it; see hinfc610_ooblayout_ops. */
#define HINFC610_OOB_PARITY			2
#define HINFC610_OOB_FREE			30
#define HINFC610_OOB_FREE_LEN		30
/*
 * Bytes the block moves for the spare on a program: the cells below its parity
 * window -- the factory mark -- plus the usable window itself. Cells 60 and up
 * have never been seen to change.
 */
#define HINFC610_OOB_XFER		(HINFC610_OOB_PARITY + HINFC610_OOB_FREE_LEN)

struct hinfc610_host {
	struct nand_controller	controller;
	struct nand_chip		chip;
	struct device			*dev;
	void __iomem			*iobase;
	void __iomem			*mmio;
	struct clk			*clk;
	struct reset_control	*reset;
	unsigned int			command;
	unsigned int			addr_cycle;
	u32				addr_value[2];
	unsigned int			offset;
	unsigned int			chipselect;
	u32				con;
	void				*buffer;
	dma_addr_t			buffer_dma;
	int				error;
	bool				uncorrectable;
};

static inline struct hinfc610_host *nand_to_hinfc610(struct nand_chip *chip)
{
	return container_of(chip, struct hinfc610_host, chip);
}

static inline unsigned int hinfc_read(struct hinfc610_host *host, unsigned int reg)
{
	return readl(host->iobase + reg);
}

static inline void hinfc_write(struct hinfc610_host *host, unsigned int value,
			       unsigned int reg)
{
	writel(value, host->iobase + reg);
}

/* An erase runs to the device's own block-erase time, not the transfer's. */
static int hinfc610_wait_erase(struct hinfc610_host *host)
{
	void __iomem *addr = host->iobase + HINFC610_STATUS;
	unsigned int val;
	int ret;

	ret = readl_poll_timeout(addr, val, val & HINFC610_STATUS_READY,
				 HINFC610_TIMEOUT_WAIT_US,
				 HINFC610_ERASE_TIMEOUT_US);
	if (ret)
		dev_err(host->dev, "timeout waiting for the erase to finish\n");

	return ret;
}

static int hinfc610_wait_ready(struct hinfc610_host *host)
{
	void __iomem *addr = host->iobase + HINFC610_STATUS;
	unsigned int val;
	int ret;

	ret = readl_poll_timeout(addr, val, val & HINFC610_STATUS_READY,
				 HINFC610_TIMEOUT_WAIT_US, HINFC610_TIMEOUT_US);
	if (ret)
		dev_err(host->dev, "timeout waiting for the controller to settle\n");

	return ret;
}

/*
 * The byte-level sequences -- read-id, status, reset -- run with the ECC field
 * cleared, which is what the vendor driver does ahead of each of them.
 */
static void hinfc610_set_con(struct hinfc610_host *host, bool ecc)
{
	u32 con = host->con;

	if (!ecc)
		con &= ~HINFC610_CON_ECCTYPE_MASK;

	hinfc_write(host, con, HINFC610_CON);
}

/*
 * Page and spare areas travel between the block and one DMA buffer of the
 * driver's, in either direction -- bit 1 of DMA_CTRL picks which. Completion
 * is polled rather than interrupted: the boot software keeps INTS_EN at zero
 * (measured), and a finished transfer shows as DMA_START cleared, with DMA_DONE
 * and any uncorrectable event latched in INTS. Both DMA addresses are loaded
 * for a program too; the parity the spare area ends up holding is the engine's
 * own, computed over the frame as it leaves.
 *
 * The pointer is the start of the spare region and stays word-aligned: aimed two
 * bytes into it, a program put the caller's bytes at cells 30 and 31, then 34 and
 * 35, and left 0x00 in the half-words between (measured), so
 * the block takes this address a word at a time.
 *
 * What it takes is not the cell order the caller names. A program of a spare
 * holding 0xff at 0..1 and 0x33 through 30..59 left 0x33 at cells 0, 1 and at
 * cells 30..57, and nothing above 57 (measured): the HINFC610_OOB_XFER bytes at
 * this pointer go to the mark cells and then to the usable window, while the
 * engine fills 2..29 with its own parity. The command hook compacts the caller's
 * cell order into that shape before starting the transfer.
 */
static int hinfc610_dma_transfer(struct hinfc610_host *host, bool todev)
{
	struct mtd_info *mtd = nand_to_mtd(&host->chip);
	void __iomem *addr = host->iobase + HINFC610_DMA_CTRL;
	unsigned int val;
	int ret;

	hinfc_write(host, HINFC610_INTCLR_ALL, HINFC610_INTCLR);
	hinfc_write(host, host->buffer_dma, HINFC610_DMA_ADDR_DATA);
	hinfc_write(host, host->buffer_dma + mtd->writesize, HINFC610_DMA_ADDR_OOB);
	hinfc_write(host, HINFC610_DMA_PARA_READ, HINFC610_DMA_PARA);

	hinfc_write(host, HINFC610_DMA_CTRL_START | HINFC610_DMA_CTRL_BURST |
		    (todev ? HINFC610_DMA_CTRL_WE : 0) |
		    (host->addr_cycle == 4 ? HINFC610_DMA_CTRL_ADDR_NUM : 0) |
		    FIELD_PREP(HINFC610_DMA_CTRL_CS, host->chipselect),
		    HINFC610_DMA_CTRL);

	ret = readl_poll_timeout(addr, val, !(val & HINFC610_DMA_CTRL_START),
				 HINFC610_TIMEOUT_WAIT_US, HINFC610_TIMEOUT_US);
	if (ret)
		dev_err(host->dev, "timeout waiting for the DMA transfer\n");

	host->uncorrectable = hinfc_read(host, HINFC610_INTS) & HINFC610_INTS_UE;
	hinfc_write(host, HINFC610_INTCLR_ALL, HINFC610_INTCLR);

	return ret;
}

/*
 * The block shifts out addr_cycle address bytes in the order the device wants
 * them: the first four come from ADDRL, any further byte from ADDRH. A page
 * operation with a column fills ADDRL exactly (two column bytes, two row
 * bytes), so its third row byte belongs in ADDRH; an erase carries no column,
 * so its three row bytes all stay inside ADDRL and it is addr_cycle that tells
 * the block to stop after three. Handing the high byte to ADDRH instead would
 * erase a different block than asked on a chip this big, and no low-offset
 * test would show it.
 */
static void hinfc610_set_addr(struct nand_chip *chip, int column, int page_addr)
{
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	u8 addr[5];
	int i, nar = 0;

	host->addr_cycle = 0;
	host->addr_value[0] = 0;
	host->addr_value[1] = 0;

	if (column != -1) {
		addr[nar++] = column & 0xff;
		addr[nar++] = (column >> 8) & 0xff;
	}

	if (page_addr != -1) {
		addr[nar++] = page_addr & 0xff;
		addr[nar++] = (page_addr >> 8) & 0xff;
		if (chip->options & NAND_ROW_ADDR_3)
			addr[nar++] = (page_addr >> 16) & 0xff;
	}

	for (i = 0; i < nar; i++) {
		if (host->addr_cycle < 4)
			host->addr_value[0] |= (u32)addr[i] << (host->addr_cycle * 8);
		else
			host->addr_value[1] |= (u32)addr[i] <<
						   ((host->addr_cycle - 4) * 8);
		host->addr_cycle++;
	}
}

/*
 * Bring one whole frame -- page plus spare, column dropped -- into the
 * driver's buffer. The read latch uses it, and so does a program asked to
 * start beyond the data area: that one still writes the frame as a unit, so it
 * has to carry the current content along.
 */
static int hinfc610_read_frame(struct hinfc610_host *host)
{
	hinfc610_set_con(host, true);
	hinfc_write(host, host->addr_value[0] & 0xffff0000, HINFC610_ADDRL);
	hinfc_write(host, host->addr_value[1], HINFC610_ADDRH);
	hinfc_write(host, HINFC610_CMD_SEQ(NAND_CMD_READ0, NAND_CMD_READSTART),
		    HINFC610_CMD);

	return hinfc610_dma_transfer(host, false);
}

/*
 * Read spare bytes straight off the device: no DMA, ECC cleared for the
 * duration, the byte count in DATA_NUM and the bytes coming out of the data
 * window. The column is the one the device expects for the spare area, which
 * is why callers pass writesize plus the position inside the spare.
 */
static int hinfc610_read_raw(struct nand_chip *chip, int page, int column,
			     u8 *buf, size_t len)
{
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	size_t i;
	int ret;

	hinfc610_set_addr(chip, column, page);
	hinfc610_set_con(host, false);
	hinfc_write(host, HINFC610_INTCLR_ALL, HINFC610_INTCLR);
	hinfc_write(host, host->addr_value[0], HINFC610_ADDRL);
	hinfc_write(host, host->addr_value[1], HINFC610_ADDRH);
	hinfc_write(host, HINFC610_CMD_SEQ(NAND_CMD_READ0, NAND_CMD_READSTART),
		    HINFC610_CMD);
	hinfc_write(host, FIELD_PREP(HINFC610_DATA_NUM_LEN_MASK, len) |
		    HINFC610_DATA_NUM_WINDOW_EN, HINFC610_DATA_NUM);
	hinfc_write(host, HINFC610_OP_READ_DATA_EN | HINFC610_OP_WAIT_READY_EN |
		    HINFC610_OP_CMD2_EN | HINFC610_OP_ADDR_EN | HINFC610_OP_CMD1_EN |
		    HINFC610_OP_NF_CS(host->chipselect) |
		    HINFC610_OP_ADDR_CYCLE(host->addr_cycle), HINFC610_OP);
	host->addr_cycle = 0;

	ret = hinfc610_wait_ready(host);
	if (ret)
		return ret;

	for (i = 0; i < len; i++)
		buf[i] = readb(host->mmio + i);

	/* The byte count belongs to the window read alone: a program that
	 * follows must not inherit it, because the vendor's program path runs
	 * with DATA_NUM left at whatever the last window operation set.
	 */
	hinfc_write(host, 0, HINFC610_DATA_NUM);

	return 0;
}

static void hinfc610_cmdfunc(struct nand_chip *chip, unsigned int command,
			     int column, int page_addr)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	u8 *spare;

	host->command = command;

	switch (command) {
	case NAND_CMD_READ0:
	case NAND_CMD_READOOB:
		if (command == NAND_CMD_READ0)
			host->offset = column;
		else
			host->offset = column + mtd->writesize;

		hinfc610_set_addr(chip, column, page_addr);
		hinfc610_read_frame(host);
		break;

	case NAND_CMD_READID:
		host->offset = column;
		hinfc610_set_con(host, false);
		hinfc_write(host, HINFC610_NANDINFO_LEN, HINFC610_DATA_NUM);
		hinfc_write(host, HINFC610_CMD_SEQ(NAND_CMD_READID, 0), HINFC610_CMD);
		hinfc_write(host, 0, HINFC610_ADDRL);
		hinfc_write(host, HINFC610_OP_CMD1_EN | HINFC610_OP_ADDR_EN |
			    HINFC610_OP_READ_DATA_EN |
			    HINFC610_OP_NF_CS(host->chipselect) |
			    HINFC610_OP_ADDR_CYCLE(1), HINFC610_OP);
		hinfc610_wait_ready(host);
		break;

	case NAND_CMD_STATUS:
		host->offset = 0;
		hinfc610_set_con(host, false);
		hinfc_write(host, HINFC610_NANDINFO_LEN, HINFC610_DATA_NUM);
		hinfc_write(host, HINFC610_CMD_SEQ(NAND_CMD_STATUS, 0), HINFC610_CMD);
		hinfc_write(host, HINFC610_OP_CMD1_EN | HINFC610_OP_READ_DATA_EN |
			    HINFC610_OP_NF_CS(host->chipselect), HINFC610_OP);
		hinfc610_wait_ready(host);
		break;

	case NAND_CMD_RESET:
		hinfc610_set_con(host, false);
		hinfc_write(host, HINFC610_CMD_SEQ(NAND_CMD_RESET, 0), HINFC610_CMD);
		hinfc_write(host, HINFC610_OP_CMD1_EN | HINFC610_OP_WAIT_READY_EN |
			    HINFC610_OP_NF_CS(host->chipselect), HINFC610_OP);
		hinfc610_wait_ready(host);
		break;

	case NAND_CMD_SEQIN:
		hinfc610_set_addr(chip, column, page_addr);
		/*
		 * The program that follows masks the column out of the address, so
		 * the frame goes out whole. A write asked to start in the spare area
		 * -- the bad-block marker is the one that matters -- therefore has to
		 * be laid over what the frame holds now, not over whatever the buffer
		 * happens to remember. An unreadable frame aborts the write through
		 * waitfunc, because the legacy cmdfunc hook returns nothing.
		 */
		if (column) {
			int ret;

			spare = (u8 *)host->buffer + mtd->writesize;
			ret = hinfc610_read_frame(host);

			if (!ret)
				ret = hinfc610_read_raw(chip, page_addr, mtd->writesize,
							spare, mtd->oobsize);
			if (ret || host->uncorrectable) {
				host->uncorrectable = false;
				host->error = -EIO;
			}
		}
		host->offset = column;
		break;

	case NAND_CMD_PAGEPROG:
		/*
		 * A caller names spare bytes by their position in the cells; the block
		 * wants them compacted -- the cells below its parity window first, then
		 * the usable window, with nothing in between and the parity region left
		 * out because the engine writes it. memmove, because the two ranges meet
		 * in the middle of this region.
		 */
		spare = (u8 *)host->buffer + mtd->writesize;
		memmove(spare + HINFC610_OOB_PARITY, spare + HINFC610_OOB_FREE,
			HINFC610_OOB_FREE_LEN);
		hinfc610_set_con(host, true);
		hinfc_write(host, host->addr_value[0] & 0xffff0000, HINFC610_ADDRL);
		hinfc_write(host, host->addr_value[1], HINFC610_ADDRH);
		/*
		 * Three command bytes, in the order the vendor sends them: take the
		 * frame, program it, then read the device status -- the block issues
		 * that last one itself.
		 */
		hinfc_write(host, HINFC610_CMD_PROGRAM, HINFC610_CMD);
		if (hinfc610_dma_transfer(host, true))
			host->error = -EIO;
		break;

	case NAND_CMD_ERASE1:
		/* Nothing reaches the bus yet: the pair goes out together. */
		hinfc610_set_addr(chip, column, page_addr);
		break;

	case NAND_CMD_ERASE2:
		/*
		 * The erase runs with the ECC field cleared, as the vendor does, and
		 * leaves it cleared: the next frame read or program rewrites CON. Only
		 * ADDRL is loaded -- the three row bytes fit in it, and the
		 * address-cycle field is what stops the block after three.
		 */
		hinfc610_set_con(host, false);
		hinfc_write(host, host->addr_value[0], HINFC610_ADDRL);
		hinfc_write(host, HINFC610_CMD_SEQ(NAND_CMD_ERASE1, NAND_CMD_ERASE2),
			    HINFC610_CMD);
		hinfc_write(host, HINFC610_OP_WAIT_READY_EN | HINFC610_OP_CMD2_EN |
			    HINFC610_OP_CMD1_EN | HINFC610_OP_ADDR_EN |
			    HINFC610_OP_NF_CS(host->chipselect) |
			    HINFC610_OP_ADDR_CYCLE(host->addr_cycle), HINFC610_OP);
		if (hinfc610_wait_erase(host))
			host->error = -EIO;
		break;

	default:
		/* Commands this block cannot put on the bus at all. */
		dev_err(host->dev, "unsupported command %u (column %d, page %d)\n",
			command, column, page_addr);
		break;
	}
}

static void hinfc610_select_chip(struct nand_chip *chip, int chipselect)
{
	struct hinfc610_host *host = nand_to_hinfc610(chip);

	if (chipselect < 0)
		return;

	host->chipselect = chipselect;
}

/*
 * The core's default wait for a controller on this legacy interface fetches
 * the status byte through read_buf, and read_buf answers from the DMA buffer
 * rather than from the device -- so the driver waits here instead: busy line
 * first, then its own status command, whose byte read_byte takes out of the
 * data window. A program or erase that failed earlier reports through the same
 * channel.
 */
static int hinfc610_waitfunc(struct nand_chip *chip)
{
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	int ret = host->error;

	host->error = 0;
	if (ret)
		return ret;

	if (hinfc610_wait_ready(host))
		return -EIO;

	hinfc610_cmdfunc(chip, NAND_CMD_STATUS, -1, -1);

	return readb(host->mmio);
}

static uint8_t hinfc610_read_byte(struct nand_chip *chip)
{
	struct hinfc610_host *host = nand_to_hinfc610(chip);

	if (host->command == NAND_CMD_STATUS)
		return readb(host->mmio);

	return readb(host->mmio + host->offset++);
}

static void hinfc610_write_buf(struct nand_chip *chip, const uint8_t *buf,
			       int len)
{
	struct hinfc610_host *host = nand_to_hinfc610(chip);

	/* Nothing reaches the bus here: the frame is assembled in the DMA buffer
	 * and handed over by the program command. The core has already kept the
	 * offset and length inside page plus spare, which is the buffer's extent.
	 */
	memcpy(host->buffer + host->offset, buf, len);
	host->offset += len;
}

static void hinfc610_read_buf(struct nand_chip *chip, uint8_t *buf, int len)
{
	struct hinfc610_host *host = nand_to_hinfc610(chip);

	memcpy(buf, host->buffer + host->offset, len);
	host->offset += len;
}

/*
 * The engine corrects eight bits per 1024-byte step, so a 2 KiB page has two
 * steps and the count register carries one byte per step in its low half.
 */
static int hinfc610_read_page(struct nand_chip *chip, uint8_t *buf,
			      int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	unsigned int stat;
	int i, corrected = 0, max_bitflips = 0;

	nand_read_page_op(chip, page, 0, buf, mtd->writesize);
	if (oob_required)
		hinfc610_read_raw(chip, page, mtd->writesize, chip->oob_poi,
				  mtd->oobsize);

	if (host->uncorrectable) {
		host->uncorrectable = false;
		mtd->ecc_stats.failed++;
		return 0;
	}

	stat = hinfc_read(host, HINFC610_ECC_STAT);
	for (i = 0; i < chip->ecc.steps; i++) {
		int flips = (stat >> (8 * i)) & 0xff;

		corrected += flips;
		max_bitflips = max(max_bitflips, flips);
	}
	mtd->ecc_stats.corrected += corrected;

	return max_bitflips;
}

/*
 * One frame, engine included: the data goes out first and the spare area
 * after it, under the ECC settings attach time installed. The parity the
 * spare ends up carrying is the engine's, so those positions do not matter.
 * The usable ones do: the frame goes out whole even when the caller supplied
 * data only, and position 0 of the spare is the factory bad-block mark, so a
 * write must not carry the previous page's bytes into it -- an erased spare
 * is filled in for that case instead.
 */
static int hinfc610_write_page(struct nand_chip *chip, const uint8_t *buf,
			       int oob_required, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	int ret;

	ret = nand_prog_page_begin_op(chip, page, 0, buf, mtd->writesize);
	if (ret)
		return ret;
	if (oob_required)
		chip->legacy.write_buf(chip, chip->oob_poi, mtd->oobsize);
	else
		memset((uint8_t *)host->buffer + mtd->writesize, 0xff,
		       mtd->oobsize);

	return nand_prog_page_end_op(chip);
}

static int hinfc610_read_oob(struct nand_chip *chip, int page)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	int ret;

	ret = hinfc610_read_raw(chip, page, mtd->writesize, chip->oob_poi,
				mtd->oobsize);
	if (ret)
		return ret;

	if (host->uncorrectable) {
		host->uncorrectable = false;
		return -EBADMSG;
	}

	return 0;
}

/*
 * Where the spare area goes on this block, measured rather than copied: a page
 * programmed with a known frame leaves twenty-eight non-erased bytes at offset 2
 * -- two steps of fourteen, exactly the engine's parity -- and everything the
 * caller supplied comes back at the position it was sent to, outside that window.
 * So parity sits at 2..29 here, not above the usable bytes as the vendor's own
 * table numbers them: that table describes the order the engine hands bytes to
 * its DMA buffer, and the raw window read -- the only way this driver looks at
 * the spare -- shows the device order, where those same bytes are 30..59. The
 * last four of the sixty-four physical bytes are never transferred by anything
 * measured here, so they are claimed by neither window.
 *
 * The factory bad-block mark is at offset 0, which is also where the core looks
 * on a page above 512 bytes (NAND_BBM_POS_LARGE). A byte written there is taken
 * as bad by the core, by the vendor system and by the board's own bootloader
 * alike -- measured by writing one and seeing all three refuse the block.
 */
static int hinfc610_ooblayout_ecc(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *region)
{
	struct nand_chip *chip = mtd_to_nand(mtd);

	if (section >= chip->ecc.steps)
		return -ERANGE;

	region->offset = HINFC610_OOB_PARITY + section * chip->ecc.bytes;
	region->length = chip->ecc.bytes;

	return 0;
}

static int hinfc610_ooblayout_free(struct mtd_info *mtd, int section,
				   struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;

	region->offset = HINFC610_OOB_FREE;
	region->length = HINFC610_OOB_FREE_LEN;

	return 0;
}

static const struct mtd_ooblayout_ops hinfc610_ooblayout_ops = {
	.ecc = hinfc610_ooblayout_ecc,
	.free = hinfc610_ooblayout_free,
};

static int hinfc610_attach_chip(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct hinfc610_host *host = nand_to_hinfc610(chip);
	size_t bufsize;

	if (chip->options & NAND_BUSWIDTH_16) {
		dev_err(host->dev, "16-bit bus width is not supported\n");
		return -EINVAL;
	}

	if (mtd->writesize != 2048) {
		dev_err(host->dev, "only 2 KiB pages are supported (found %d)\n",
			mtd->writesize);
		return -EINVAL;
	}

	if (chip->ecc.engine_type != NAND_ECC_ENGINE_TYPE_ON_HOST) {
		dev_err(host->dev, "the block is only driven with its own ECC engine\n");
		return -EINVAL;
	}

	/*
	 * One strength is implemented because it is the one this board has ever been
	 * read with: the vendor system's live CON carries ECCTYPE 1, and a request
	 * reaches the driver as the pair the device tree states.
	 */
	if (chip->ecc.size != 1024 || chip->ecc.strength != 8) {
		dev_err(host->dev,
			"unsupported ECC request: %d bits per %d bytes (this block does 8 per 1024)\n",
			chip->ecc.strength, chip->ecc.size);
		return -EINVAL;
	}

	/*
	 * The spare area is not fetched through the DMA that carries a page: with
	 * the engine active the block hands back the data area and keeps the parity
	 * for itself, so the buffer's spare half would only echo the previous
	 * transfer (measured: an erased block reported the parity of the page read
	 * before it, and a programmed one reported the 0xFF this driver had just
	 * filled in). Every spare byte a caller sees therefore comes from the raw
	 * window read below, which is also what the vendor's own bad-block check
	 * uses. The buffer still spans page plus spare because that is what a
	 * program sends.
	 */
	bufsize = mtd->writesize + mtd->oobsize;
	host->buffer = dmam_alloc_coherent(host->dev, bufsize, &host->buffer_dma,
					   GFP_KERNEL);
	if (!host->buffer)
		return -ENOMEM;

	/* Erased from the start: a program sends the whole frame, and the bytes a
	 * caller did not supply should read as blank rather than as pool content.
	 */
	memset(host->buffer, 0xff, bufsize);

	chip->ecc.bytes = 14;
	chip->ecc.total = chip->ecc.steps * chip->ecc.bytes;
	chip->ecc.read_page = hinfc610_read_page;
	chip->ecc.read_oob = hinfc610_read_oob;
	chip->ecc.write_page = hinfc610_write_page;
	mtd_set_ooblayout(mtd, &hinfc610_ooblayout_ops);

	host->con |= FIELD_PREP(HINFC610_CON_ECCTYPE_MASK,
				HINFC610_CON_ECCTYPE_8BIT_1K);

	return 0;
}

static const struct nand_controller_ops hinfc610_controller_ops = {
	.attach_chip = hinfc610_attach_chip,
};

static int hinfc610_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nand_chip *chip;
	struct mtd_info *mtd;
	struct hinfc610_host *host;
	unsigned int version;
	u32 con;
	int ret;

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	host->dev = dev;
	chip = &host->chip;
	mtd = nand_to_mtd(chip);

	host->iobase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->iobase))
		return PTR_ERR(host->iobase);

	host->mmio = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(host->mmio))
		return PTR_ERR(host->mmio);

	/*
	 * The gate and the reset are one CRG word apart and the reset has to be
	 * asked for by name, so both are taken before anything touches the
	 * block: a reset requested while the clock is gated cannot be released.
	 */
	host->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(host->clk))
		return dev_err_probe(dev, PTR_ERR(host->clk), "failed to get clock\n");

	host->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(host->reset))
		return dev_err_probe(dev, PTR_ERR(host->reset),
				     "failed to get the block reset\n");

	version = hinfc_read(host, HINFC610_VERSION);
	if (version != HINFC610_VERSION_610) {
		dev_err(dev, "unsupported controller version %#x\n", version);
		return -ENODEV;
	}

	nand_controller_init(&host->controller);
	host->controller.ops = &hinfc610_controller_ops;
	chip->controller = &host->controller;
	nand_set_controller_data(chip, host);
	nand_set_flash_node(chip, dev->of_node);

	/*
	 * Nothing the boot software left in this block is trusted: hold it in
	 * reset, release it, and configure from the reset state instead of on top of
	 * the hand-over state. The reset named by the node is the block's own soft
	 * reset (nf_srst_req, hi_reg_crg.h:357 for this chip).
	 */
	ret = reset_control_assert(host->reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to hold the block in reset\n");

	ret = reset_control_deassert(host->reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to release the block reset\n");

	/*
	 * Composed from scratch rather than inherited: the boot software leaves bit
	 * 7 set (measured 0x383) and neither vendor tree names it, so it is left
	 * out here.
	 *
	 * The page size is set here rather than after the chip has been found,
	 * because the identification commands run before that: with the field left
	 * at its reset value of zero the read-id comes back with nothing
	 * recognisable in the window and the core says "nand: No NAND device
	 * found" (measured on this board), while the boot software has the field at
	 * 1 whenever it has touched the block. Only the ECC field is left cleared
	 * until attach time, since a read-id has to run without the engine.
	 */
	con = HINFC610_CON_OP_MODE_NORMAL | HINFC610_CON_READY_BUSY_SEL |
	      FIELD_PREP(HINFC610_CON_PAGESIZE_MASK, HINFC610_CON_PAGESIZE_2K);
	host->con = con;

	hinfc_write(host, host->con, HINFC610_CON);
	hinfc_write(host, HINFC610_PWIDTH_S40, HINFC610_PWIDTH);

	mtd->name = "hinand";
	mtd->dev.parent = dev;

	chip->legacy.cmdfunc = hinfc610_cmdfunc;
	chip->legacy.select_chip = hinfc610_select_chip;
	chip->legacy.read_byte = hinfc610_read_byte;
	chip->legacy.read_buf = hinfc610_read_buf;
	chip->legacy.write_buf = hinfc610_write_buf;
	chip->legacy.waitfunc = hinfc610_waitfunc;
	chip->legacy.chip_delay = HINFC610_CHIP_DELAY;
	chip->legacy.set_features = nand_get_set_features_notsupp;
	chip->legacy.get_features = nand_get_set_features_notsupp;

	platform_set_drvdata(pdev, host);

	ret = nand_scan(chip, HINFC610_MAX_CHIP);
	if (ret)
		return ret;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret)
		nand_cleanup(chip);

	return ret;
}

static void hinfc610_remove(struct platform_device *pdev)
{
	struct hinfc610_host *host = platform_get_drvdata(pdev);

	if (mtd_device_unregister(nand_to_mtd(&host->chip)))
		dev_err(&pdev->dev, "failed to unregister MTD\n");

	nand_cleanup(&host->chip);
}

static const struct of_device_id hinfc610_of_match[] = {
	{ .compatible = "hisilicon,hi3716cv200-nand-controller" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, hinfc610_of_match);

static struct platform_driver hinfc610_driver = {
	.probe = hinfc610_probe,
	.remove = hinfc610_remove,
	.driver = {
		.name = "hinfc610",
		.of_match_table = hinfc610_of_match,
	},
};
module_platform_driver(hinfc610_driver);

MODULE_AUTHOR("Hisilicon Company Limited");
MODULE_DESCRIPTION("Hisilicon NAND Flash Controller V610 driver");
MODULE_LICENSE("GPL");
