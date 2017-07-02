/*
    NitroHax -- Cheat tool for the Nintendo DS
    Copyright (C) 2008  Michael "Chishm" Chisholm

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <nds.h> 
#include <nds/fifomessages.h>
#include "cardEngine.h"

#define READ_SIZE_ARM7 0x1000
#define SLOT_SIZE      0x8000

#define CACHE_ADRESS_START 0x03708000
#define CACHE_ADRESS_SIZE 0x78000
#define REG_MBK_CACHE_START	0x4004045
#define REG_MBK_SLOT_SIZE	15
#define REG_MBK_CACHE_SIZE	120

extern vu32* volatile cardStruct;
//extern vu32* volatile cacheStruct;
extern u32 sdk_version;
extern u32 needFlushDCCache;
vu32* volatile sharedAddr = (vu32*)0x027FFB08;
extern volatile int (*readCachedRef)(u32*); // this pointer is not at the end of the table but at the handler pointer corresponding to the current irq

static u32 cacheDescriptor [REG_MBK_CACHE_SIZE];
static u32 cacheCounter [REG_MBK_CACHE_SIZE];
static u32 accessCounter = 0;

static u32 asyncSector = 0;
static u32 currentSlot = 0;
static u32 asyncQueue [5];
static int aQHead = 0;
static int aQTail = 0;
static int aQSize = 0;

bool isSlotAccessibleFromArm9(int slot) {
	return (*((vu8*)(REG_MBK_CACHE_START+(slot/8))) & 0x1 == 0);
}

int allocateCacheSlot() {
	int slot = 0;
	u32 lowerCounter = accessCounter;
	for(int i=0; i<REG_MBK_CACHE_SIZE; i++) {
		if((currentSlot/8) == (i/8) || !isSlotAccessibleFromArm9(i)) {			
			i = (i/8) * 8 + 8 - 1;
			if(i>=REG_MBK_CACHE_SIZE) break;
		} else {
			if(cacheCounter[i]<=lowerCounter) {
				lowerCounter = cacheCounter[i];
				slot = i;
				if(!lowerCounter) break;
			}
		}		
	}
	return slot;
}

int getSlotForSector(u32 sector) {
	for(int i=0; i<REG_MBK_CACHE_SIZE; i++) {
		if(cacheDescriptor[i]==sector) {
			return i;
		}
	}
	return -1;
}


vu8* getCacheAddress(int slot) {
	return (vu32*)(CACHE_ADRESS_START+slot*READ_SIZE_ARM7);
}

void transfertToArm7(int slot) {
	*((vu8*)(REG_MBK_CACHE_START+(slot/8))) |= 0x1;
}

void transfertToArm9(int slot) {
	*((vu8*)(REG_MBK_CACHE_START+(slot/8))) &= 0xFE;
}

void updateDescriptor(int slot, u32 sector) {
	cacheDescriptor[slot] = sector;
	cacheCounter[slot] = accessCounter;
}

void addToAsyncQueue(sector) {
	asyncQueue[aQHead] = sector;
	aQHead++;
	aQSize++;
	if(aQHead>4) {
		aQHead=0;
	}
	if(aQSize>5) {
		aQSize=5;
		aQTail++;
		if(aQTail>4) aQTail=0;
	}
}

u32 popFromAsyncQueueHead() {	
	if(aQSize>0) {
	
		aQHead--;
		if(aQHead == -1) aQHead = 4;
		aQSize--;
		
		return asyncQueue[aQHead];
	} else return 0;
}

void triggerAsyncPrefetch(sector) {	
	if(asyncSector == 0) {
		int slot = getSlotForSector(sector);
		// read max 32k via the WRAM cache
		// do it only if there is no async command ongoing
		if(slot==-1) {
			addToAsyncQueue(sector);
			// send a command to the arm7 to fill the WRAM cache
			u32 commandRead = 0x020ff800;		
			
			slot = allocateCacheSlot();
			
			vu8* buffer = getCacheAddress(slot);
			
			if(needFlushDCCache) DC_FlushRange(buffer, READ_SIZE_ARM7);
			
			// transfer the WRAM-B cache to the arm7
			transfertToArm7(slot);		

			cacheDescriptor[slot] = sector;
			cacheCounter[slot] = 0x0FFFFFFF ; // async marker
			asyncSector = sector;		
			
			// write the command
			sharedAddr[0] = buffer;
			sharedAddr[1] = READ_SIZE_ARM7;
			sharedAddr[2] = sector;
			sharedAddr[3] = commandRead;
			
			IPC_SendSync(0xEE24);			


			// do it asynchronously
			/*while(sharedAddr[3] != (vu32)0);	
			
			// transfer back the WRAM-B cache to the arm9
			//transfertToArm9(slot);*/
		}	
	}	
}

void processAsyncCommand() {
	if(asyncSector != 0) {
		int slot = getSlotForSector(asyncSector);
		if(slot!=-1 && cacheCounter[slot] == 0x0FFFFFFF) {
			// get back the data from arm7
			if(sharedAddr[3] == (vu32)0) {
				// transfer back the WRAM-B cache to the arm9
				transfertToArm9(slot);		
				
				updateDescriptor(slot, asyncSector);
				asyncSector = 0;
			}			
		}	
	}
}

void getAsyncSector() {
	if(asyncSector != 0) {
		int slot = getSlotForSector(asyncSector);
		if(slot!=-1 && cacheCounter[slot] == 0x0FFFFFFF) {
			// get back the data from arm7
			while(sharedAddr[3] != (vu32)0);
			
			// transfer back the WRAM-B cache to the arm9
			transfertToArm9(slot);		
			
			updateDescriptor(slot, asyncSector);
			asyncSector = 0;
		}	
	}	
}

void cardRead (u32* cacheStruct) {
	//nocashMessage("\narm9 cardRead\n");	
	
	accessCounter++;
	
	u8* cacheBuffer = (u8*)(cacheStruct + 8);
	u32* cachePage = cacheStruct + 2;
	u32 commandRead;
	u32 src = cardStruct[0];
	u8* dst = (u8*) (cardStruct[1]);
	u32 len = cardStruct[2];
	
	u32 page = (src/512)*512;
	
	u32 sector = (src/READ_SIZE_ARM7)*READ_SIZE_ARM7;
	
	#ifdef DEBUG
	// send a log command for debug purpose
	// -------------------------------------
	commandRead = 0x026ff800;	
	
	sharedAddr[0] = dst;
	sharedAddr[1] = len;
	sharedAddr[2] = src;
	sharedAddr[3] = commandRead;
	
	IPC_SendSync(0xEE24);
	
	while(sharedAddr[3] != (vu32)0);
	// -------------------------------------*/
	#endif
	
	processAsyncCommand();
	
	if(page == src && len > READ_SIZE_ARM7 && dst < 0x02700000 && dst > 0x02000000 && ((u32)dst)%4==0) {
		getAsyncSector();
		
		// read directly at arm7 level
		commandRead = 0x025FFB08;
		
		cacheFlush();
		
		sharedAddr[0] = dst;
		sharedAddr[1] = len;
		sharedAddr[2] = src;
		sharedAddr[3] = commandRead;
		
		IPC_SendSync(0xEE24);
		
		while(sharedAddr[3] != (vu32)0);
		
	} else {
		// read via the WRAM cache
		while(len > 0) {
			int slot = getSlotForSector(sector);
			vu8* buffer = getCacheAddress(slot);
			u32 nextSector = sector+READ_SIZE_ARM7;	

			// read max 32k via the WRAM cache
			if(slot==-1) {
				getAsyncSector();
				
				// send a command to the arm7 to fill the WRAM cache
				commandRead = 0x025FFB08;
				
				slot = allocateCacheSlot();
				currentSlot = slot;
				
				buffer = getCacheAddress(slot);
				
				if(needFlushDCCache) DC_FlushRange(buffer, READ_SIZE_ARM7);
				
				// transfer the WRAM-B cache to the arm7
				transfertToArm7(slot);				
				
				// write the command
				sharedAddr[0] = buffer;
				sharedAddr[1] = READ_SIZE_ARM7;
				sharedAddr[2] = sector;
				sharedAddr[3] = commandRead;
				
				IPC_SendSync(0xEE24);	

				while(sharedAddr[3] != (vu32)0);	
				
				// transfer back the WRAM-B cache to the arm9
				transfertToArm9(slot);		
				updateDescriptor(slot, sector);				
	
				triggerAsyncPrefetch(nextSector);		
			} else {
				currentSlot = slot;
				if(cacheCounter[slot] == 0x0FFFFFFF || !isSlotAccessibleFromArm9(slot)) {
					// prefetch successfull
					getAsyncSector();
					
					triggerAsyncPrefetch(nextSector);	
				} else {
					int i;
					for(i=0; i<5; i++) {
						if(asyncQueue[i]==sector) {
							// prefetch successfull
							triggerAsyncPrefetch(nextSector);	
							break;
						}
					}
				}
				updateDescriptor(slot, sector);
			}
			
			
			u32 len2=len;
			if((src - sector) + len2 > READ_SIZE_ARM7){
			    len2 = sector - src + READ_SIZE_ARM7;
			}
			
			if(len2 > 512) {
				len2 -= src%4;
				len2 -= len2 % 32;
			}

			if(len2 >= 512 && len2 % 32 == 0 && ((u32)dst)%4 == 0 && src%4 == 0) {		
				#ifdef DEBUG		
				// send a log command for debug purpose
				// -------------------------------------
				commandRead = 0x026ff800;	
				
				sharedAddr[0] = dst;
				sharedAddr[1] = len2;
				sharedAddr[2] = buffer+src-sector;
				sharedAddr[3] = commandRead;
				
				IPC_SendSync(0xEE24);
				
				while(sharedAddr[3] != (vu32)0);
				// -------------------------------------*/
				#endif
			
				// copy directly
				fastCopy32(buffer+(src-sector),dst,len2);	
				
				// update cardi common
				cardStruct[0] = src + len2;
				cardStruct[1] = dst + len2;
				cardStruct[2] = len - len2;
			} else {				
				#ifdef DEBUG		
				// send a log command for debug purpose
				// -------------------------------------
				commandRead = 0x026ff800;	
				
				sharedAddr[0] = page;
				sharedAddr[1] = len2;
				sharedAddr[2] = buffer+page-sector;
				sharedAddr[3] = commandRead;
				
				IPC_SendSync(0xEE24);
				
				while(sharedAddr[3] != (vu32)0);
				// -------------------------------------*/
				#endif
					
				// read via the 512b ram cache
				fastCopy32(buffer+(page-sector), cacheBuffer, 512);
				*cachePage = page;
				(*readCachedRef)(cacheStruct);
			}
			len = cardStruct[2];
			if(len>0) {
				src = cardStruct[0];
				dst = cardStruct[1];
				page = (src/512)*512;
				sector = (src/READ_SIZE_ARM7)*READ_SIZE_ARM7;
				accessCounter++;
			}			
		}
	}	
}




