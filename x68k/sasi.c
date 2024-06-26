/*
 *  SASI.C - Shugart Associates System Interface (SASI HDD)
 */

#include "common.h"
#include "../libretro/dosio.h"
#include "prop.h"
#include "status.h"
#include "../m68000/m68000.h"
#include "ioc.h"
#include "sasi.h"
#include "irqh.h"

static uint8_t SASI_Buf[256];
static uint8_t SASI_Phase        = 0;
static uint32_t SASI_Sector      = 0;
static uint32_t SASI_Blocks      = 0;
static uint8_t SASI_Cmd[6];
static uint8_t SASI_CmdPtr       = 0;
static uint16_t SASI_Device      = 0;
static uint8_t SASI_Unit         = 0;
static int16_t SASI_BufPtr       = 0;
static uint8_t SASI_RW           = 0;
static uint8_t SASI_Stat         = 0;
static uint8_t SASI_Error        = 0;
static uint8_t SASI_SenseStatBuf[4];
static uint8_t SASI_SenseStatPtr = 0;

int SASI_IsReady(void)
{
	if ( (SASI_Phase==2)||(SASI_Phase==3)||(SASI_Phase==9) )
		return 1;
	return 0;
}

static uint32_t FASTCALL SASI_Int(uint8_t irq)
{
	IRQH_IRQCallBack(irq);
	if (irq==1)
		return ((uint32_t)IOC_IntVect+2);
	return -1;
}

void SASI_Init(void)
{
	SASI_Phase = 0;
	SASI_Sector = 0;
	SASI_Blocks = 0;
	SASI_CmdPtr = 0;
	SASI_Device = 0;
	SASI_Unit = 0;
	SASI_BufPtr = 0;
	SASI_RW = 0;
	SASI_Stat = 0;
	SASI_Error = 0;
	SASI_SenseStatPtr = 0;
}

static int16_t SASI_Seek(void)
{
	void *fp;

	memset(SASI_Buf, 0, 256);
	if (!(fp = file_open(Config.HDImage[SASI_Device*2+SASI_Unit])))
	{
		memset(SASI_Buf, 0, 256);
		return -1;
	}
	if (file_seek(fp, SASI_Sector<<8, FSEEK_SET)!=(SASI_Sector<<8)) 
		goto error;
	if (file_lread(fp, SASI_Buf, 256)!=256)
		goto error;
	file_close(fp);

	return 1;

error:
	file_close(fp);
	return 0;
}

static int16_t SASI_Flush(void)
{
	void *fp = file_open(Config.HDImage[SASI_Device*2+SASI_Unit]);
	if (!fp) return -1;
	if (file_seek(fp, SASI_Sector<<8, FSEEK_SET)!=(SASI_Sector<<8))
		goto error;
	if (file_lwrite(fp, SASI_Buf, 256)!=256)
		goto error;
	file_close(fp);

	return 1;

error:
	file_close(fp);
	return 0;
}

/*
 *   I/O Read
 */
uint8_t FASTCALL SASI_Read(uint32_t adr)
{
	uint8_t ret = 0;
	int16_t result;

	if (adr==0xe96003)
	{
		if (SASI_Phase)
			ret |= 2;		/* Busy */
		if (SASI_Phase>1)
			ret |= 1;		/* Req  */
		if (SASI_Phase==2)
			ret |= 8;		/* C/D  */
		if ((SASI_Phase==3)&&(SASI_RW))	/* SASI_RW=1:Read */
			ret |= 4;		/* I/O */
		if (SASI_Phase==9)		/* Phase=9:SenseStatus�� */
			ret |= 4;		/* I/O */
		if ((SASI_Phase==4)||(SASI_Phase==5))
			ret |= 0x0c;		/* I/O & C/D */
		if (SASI_Phase==5)
			ret |= 0x10;		/* MSG */
	}
	else if (adr ==0xe96001)
	{
		if ((SASI_Phase==3)&&(SASI_RW))
		{
			ret = SASI_Buf[SASI_BufPtr++];
			if (SASI_BufPtr==256)
			{
				SASI_Blocks--;
				if (SASI_Blocks)
				{
					SASI_Sector++;
					SASI_BufPtr = 0;
					result = SASI_Seek();
					if (!result)
					{
						SASI_Error = 0x0f;
						SASI_Phase++;
					}
				}
				else
					SASI_Phase++;
			}
		}
		else if (SASI_Phase==4) /* Status Phase */
		{
			if (SASI_Error)
				ret = 0x02;
			else
				ret = SASI_Stat;
			SASI_Phase++;
		}
		else if (SASI_Phase==5) /* MessagePhase */
			SASI_Phase = 0;
		else if (SASI_Phase==9)	/* DataPhase(SenseStat����) */
		{
			ret = SASI_SenseStatBuf[SASI_SenseStatPtr++];
			if (SASI_SenseStatPtr==4)
			{
				SASI_Error = 0;
				SASI_Phase = 4;	/* StatusPhase�� */
			}
		}
		if (SASI_Phase==4)
		{
			IOC_IntStat|=0x10;
			if (IOC_IntStat&8) IRQH_Int(1, &SASI_Int);
		}
	}

	StatBar_HDD((SASI_Phase)?2:0);

	return ret;
}

static void SASI_CheckCmd(void)
{
	int16_t result;
	SASI_Unit = (SASI_Cmd[1]>>5) & 1;

	switch(SASI_Cmd[0])
   {
      case 0x00:					/* Test Drive Ready */
         if (Config.HDImage[SASI_Device*2+SASI_Unit][0])
            SASI_Stat = 0;
         else
         {
            SASI_Stat = 0x02;
            SASI_Error = 0x7f;
         }
         SASI_Phase += 2;
         break;
      case 0x01: /* Recalibrate */
         if (Config.HDImage[SASI_Device*2+SASI_Unit][0])
         {
            SASI_Sector = 0;
            SASI_Stat = 0;
         }
         else
         {
            SASI_Stat = 0x02;
            SASI_Error = 0x7f;
         }
         SASI_Phase += 2;
         break;
      case 0x03: /* Request Sense Status */
         SASI_SenseStatBuf[0] = SASI_Error;
         SASI_SenseStatBuf[1] = (uint8_t)((SASI_Unit<<5)|((SASI_Sector>>16)&0x1f));
         SASI_SenseStatBuf[2] = (uint8_t)(SASI_Sector>>8);
         SASI_SenseStatBuf[3] = (uint8_t)SASI_Sector;
         SASI_Error = 0;
         SASI_Phase=9;
         SASI_Stat = 0;
         SASI_SenseStatPtr = 0;
         break;
      case 0x04: /* Format Drive */
         SASI_Phase += 2;
         SASI_Stat = 0;
         break;
      case 0x08:					/* Read Data */
         SASI_Sector = (((uint32_t)SASI_Cmd[1]&0x1f)<<16)|(((uint32_t)SASI_Cmd[2])<<8)|((uint32_t)SASI_Cmd[3]);
         SASI_Blocks = (uint32_t)SASI_Cmd[4];
         SASI_Phase++;
         SASI_RW = 1;
         SASI_BufPtr = 0;
         SASI_Stat = 0;
         result = SASI_Seek();
         if ( (result==0)||(result==-1) )
            SASI_Error = 0x0f;
         break;
      case 0x0a:					/* Write Data */
         SASI_Sector = (((uint32_t)SASI_Cmd[1]&0x1f)<<16)|(((uint32_t)SASI_Cmd[2])<<8)|((uint32_t)SASI_Cmd[3]);
         SASI_Blocks = (uint32_t)SASI_Cmd[4];
         SASI_Phase++;
         SASI_RW = 0;
         SASI_BufPtr = 0;
         SASI_Stat = 0;
         memset(SASI_Buf, 0, 256);
         result = SASI_Seek();
         if ( (result==0)||(result==-1) )
            SASI_Error = 0x0f;
         break;
      case 0x0b:					/* Seek */
         if (Config.HDImage[SASI_Device*2+SASI_Unit][0])
            SASI_Stat = 0;
         else
         {
            SASI_Stat = 0x02;
            SASI_Error = 0x7f;
         }
         SASI_Phase += 2;
         break;
      case 0xc2:
         SASI_Phase = 10;
         SASI_SenseStatPtr = 0;
         if (Config.HDImage[SASI_Device*2+SASI_Unit][0])
            SASI_Stat = 0;
         else
         {
            SASI_Stat = 0x02;
            SASI_Error = 0x7f;
         }
         break;
      default:
         SASI_Phase += 2;
   }
}


/*
 *   I/O Write
 */
void FASTCALL SASI_Write(uint32_t adr, uint8_t data)
{
	int16_t result;
	int i;
	uint8_t bit;

	if ( (adr==0xe96007)&&(SASI_Phase==0) )
	{
		SASI_Device = 0x7f;
		if (data)
		{
			for (i=0, bit=1; bit; i++, bit<<=1)
			{
				if (data&bit)
				{
					SASI_Device = i;
					break;
				}
			}
		}
		if ( (Config.HDImage[SASI_Device*2][0])||(Config.HDImage[SASI_Device*2+1][0]) )
		{
			SASI_Phase++;
			SASI_CmdPtr = 0;
		}
		else
			SASI_Phase = 0;
	}
	else if ( (adr==0xe96003)&&(SASI_Phase==1) )
		SASI_Phase++;
	else if (adr==0xe96005) /* SASI Reset */
	{
		SASI_Phase = 0;
		SASI_Sector = 0;
		SASI_Blocks = 0;
		SASI_CmdPtr = 0;
		SASI_Device = 0;
		SASI_Unit = 0;
		SASI_BufPtr = 0;
		SASI_RW = 0;
		SASI_Stat = 0;
		SASI_Error = 0;
		SASI_SenseStatPtr = 0;
	}
	else if (adr==0xe96001)
	{
		if (SASI_Phase==2)
		{
			SASI_Cmd[SASI_CmdPtr++] = data;
			if (SASI_CmdPtr==6)
				SASI_CheckCmd();
		}
		else if ((SASI_Phase==3) && (!SASI_RW))
		{
			SASI_Buf[SASI_BufPtr++] = data;
			if (SASI_BufPtr==256)
			{
				result = SASI_Flush();
				SASI_Blocks--;
				if (SASI_Blocks)
				{
					SASI_Sector++;
					SASI_BufPtr = 0;
					result = SASI_Seek();
					if (!result)
					{
						SASI_Error = 0x0f;
						SASI_Phase++;
					}
				}
				else
					SASI_Phase++;
			}
		}
		else if (SASI_Phase==10)
		{
			SASI_SenseStatPtr++;
			if (SASI_SenseStatPtr==10)
				SASI_Phase = 4;
		}
		if (SASI_Phase==4)
		{
			IOC_IntStat|=0x10;
			if (IOC_IntStat&8) IRQH_Int(1, &SASI_Int);
		}
	}
	StatBar_HDD((SASI_Phase)?2:0);
}
