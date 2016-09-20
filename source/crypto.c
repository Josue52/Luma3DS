/*
*   This file is part of Luma3DS
*   Copyright (C) 2016 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b of GPLv3 applies to this file: Requiring preservation of specified
*   reasonable legal notices or author attributions in that material or in the Appropriate Legal
*   Notices displayed by works containing it.
*/

/*
*   Crypto libs from http://github.com/b1l1s/ctr
*   ARM9Loader code originally adapted from https://github.com/Reisyukaku/ReiNand/blob/228c378255ba693133dec6f3368e14d386f2cde7/source/crypto.c#L233
*   decryptNusFirm code adapted from https://github.com/mid-kid/CakesForeveryWan/blob/master/source/firm.c
*/

#include "crypto.h"
#include "memory.h"
#include "fatfs/sdmmc/sdmmc.h"

/****************************************************************
*                  Crypto libs
****************************************************************/

/* original version by megazig */

#ifndef __thumb__
#define BSWAP32(x) {\
    __asm__\
    (\
        "eor r1, %1, %1, ror #16\n\t"\
        "bic r1, r1, #0xFF0000\n\t"\
        "mov %0, %1, ror #8\n\t"\
        "eor %0, %0, r1, lsr #8\n\t"\
        :"=r"(x)\
        :"0"(x)\
        :"r1"\
    );\
};

#define ADD_u128_u32(u128_0, u128_1, u128_2, u128_3, u32_0) {\
__asm__\
    (\
        "adds %0, %4\n\t"\
        "addcss %1, %1, #1\n\t"\
        "addcss %2, %2, #1\n\t"\
        "addcs %3, %3, #1\n\t"\
        : "+r"(u128_0), "+r"(u128_1), "+r"(u128_2), "+r"(u128_3)\
        : "r"(u32_0)\
        : "cc"\
    );\
}
#else
#define BSWAP32(x) {x = __builtin_bswap32(x);}

#define ADD_u128_u32(u128_0, u128_1, u128_2, u128_3, u32_0) {\
__asm__\
    (\
        "mov r4, #0\n\t"\
        "add %0, %0, %4\n\t"\
        "adc %1, %1, r4\n\t"\
        "adc %2, %2, r4\n\t"\
        "adc %3, %3, r4\n\t"\
        : "+r"(u128_0), "+r"(u128_1), "+r"(u128_2), "+r"(u128_3)\
        : "r"(u32_0)\
        : "cc", "r4"\
    );\
}
#endif /*__thumb__*/

static void aes_setkey(u8 keyslot, const void *key, u32 keyType, u32 mode)
{
    if(keyslot <= 0x03) return; // Ignore TWL keys for now
    u32 *key32 = (u32 *)key;
    *REG_AESCNT = (*REG_AESCNT & ~(AES_CNT_INPUT_ENDIAN | AES_CNT_INPUT_ORDER)) | mode;
    *REG_AESKEYCNT = (*REG_AESKEYCNT >> 6 << 6) | keyslot | AES_KEYCNT_WRITE;

    REG_AESKEYFIFO[keyType] = key32[0];
    REG_AESKEYFIFO[keyType] = key32[1];
    REG_AESKEYFIFO[keyType] = key32[2];
    REG_AESKEYFIFO[keyType] = key32[3];
}

static void aes_use_keyslot(u8 keyslot)
{
    if(keyslot > 0x3F)
        return;

    *REG_AESKEYSEL = keyslot;
    *REG_AESCNT = *REG_AESCNT | 0x04000000; /* mystery bit */
}

static void aes_setiv(const void *iv, u32 mode)
{
    const u32 *iv32 = (const u32 *)iv;
    *REG_AESCNT = (*REG_AESCNT & ~(AES_CNT_INPUT_ENDIAN | AES_CNT_INPUT_ORDER)) | mode;

    // Word order for IV can't be changed in REG_AESCNT and always default to reversed
    if(mode & AES_INPUT_NORMAL)
    {
        REG_AESCTR[0] = iv32[3];
        REG_AESCTR[1] = iv32[2];
        REG_AESCTR[2] = iv32[1];
        REG_AESCTR[3] = iv32[0];
    }
    else
    {
        REG_AESCTR[0] = iv32[0];
        REG_AESCTR[1] = iv32[1];
        REG_AESCTR[2] = iv32[2];
        REG_AESCTR[3] = iv32[3];
    }
}

static void aes_advctr(void *ctr, u32 val, u32 mode)
{
    u32 *ctr32 = (u32 *)ctr;
    
    int i;
    if(mode & AES_INPUT_BE)
    {
        for(i = 0; i < 4; ++i) // Endian swap
            BSWAP32(ctr32[i]);
    }
    
    if(mode & AES_INPUT_NORMAL)
    {
        ADD_u128_u32(ctr32[3], ctr32[2], ctr32[1], ctr32[0], val);
    }
    else
    {
        ADD_u128_u32(ctr32[0], ctr32[1], ctr32[2], ctr32[3], val);
    }
    
    if(mode & AES_INPUT_BE)
    {
        for(i = 0; i < 4; ++i) // Endian swap
            BSWAP32(ctr32[i]);
    }
}

static void aes_change_ctrmode(void *ctr, u32 fromMode, u32 toMode)
{
    u32 *ctr32 = (u32 *)ctr;
    int i;
    if((fromMode ^ toMode) & AES_CNT_INPUT_ENDIAN)
    {
        for(i = 0; i < 4; ++i)
            BSWAP32(ctr32[i]);
    }

    if((fromMode ^ toMode) & AES_CNT_INPUT_ORDER)
    {
        u32 temp = ctr32[0];
        ctr32[0] = ctr32[3];
        ctr32[3] = temp;

        temp = ctr32[1];
        ctr32[1] = ctr32[2];
        ctr32[2] = temp;
    }
}

static void aes_batch(void *dst, const void *src, u32 blockCount)
{
    *REG_AESBLKCNT = blockCount << 16;
    *REG_AESCNT |=  AES_CNT_START;
    
    const u32 *src32    = (const u32 *)src;
    u32 *dst32          = (u32 *)dst;
    
    u32 wbc = blockCount;
    u32 rbc = blockCount;
    
    while(rbc)
    {
        if(wbc && ((*REG_AESCNT & 0x1F) <= 0xC)) // There's space for at least 4 ints
        {
            *REG_AESWRFIFO = *src32++;
            *REG_AESWRFIFO = *src32++;
            *REG_AESWRFIFO = *src32++;
            *REG_AESWRFIFO = *src32++;
            wbc--;
        }
        
        if(rbc && ((*REG_AESCNT & (0x1F << 0x5)) >= (0x4 << 0x5))) // At least 4 ints available for read
        {
            *dst32++ = *REG_AESRDFIFO;
            *dst32++ = *REG_AESRDFIFO;
            *dst32++ = *REG_AESRDFIFO;
            *dst32++ = *REG_AESRDFIFO;
            rbc--;
        }
    }
}

static void aes(void *dst, const void *src, u32 blockCount, void *iv, u32 mode, u32 ivMode)
{
    *REG_AESCNT =   mode |
                    AES_CNT_INPUT_ORDER | AES_CNT_OUTPUT_ORDER |
                    AES_CNT_INPUT_ENDIAN | AES_CNT_OUTPUT_ENDIAN |
                    AES_CNT_FLUSH_READ | AES_CNT_FLUSH_WRITE;

    u32 blocks;
    while(blockCount != 0)
    {
        if((mode & AES_ALL_MODES) != AES_ECB_ENCRYPT_MODE
        && (mode & AES_ALL_MODES) != AES_ECB_DECRYPT_MODE)
            aes_setiv(iv, ivMode);

        blocks = (blockCount >= 0xFFFF) ? 0xFFFF : blockCount;

        // Save the last block for the next decryption CBC batch's iv
        if((mode & AES_ALL_MODES) == AES_CBC_DECRYPT_MODE)
        {
            memcpy(iv, src + (blocks - 1) * AES_BLOCK_SIZE, AES_BLOCK_SIZE);
            aes_change_ctrmode(iv, AES_INPUT_BE | AES_INPUT_NORMAL, ivMode);
        }

        // Process the current batch
        aes_batch(dst, src, blocks);

        // Save the last block for the next encryption CBC batch's iv
        if((mode & AES_ALL_MODES) == AES_CBC_ENCRYPT_MODE)
        {
            memcpy(iv, dst + (blocks - 1) * AES_BLOCK_SIZE, AES_BLOCK_SIZE);
            aes_change_ctrmode(iv, AES_INPUT_BE | AES_INPUT_NORMAL, ivMode);
        }
        
        // Advance counter for CTR mode
        else if((mode & AES_ALL_MODES) == AES_CTR_MODE)
            aes_advctr(iv, blocks, ivMode);

        src += blocks * AES_BLOCK_SIZE;
        dst += blocks * AES_BLOCK_SIZE;
        blockCount -= blocks;
    }
}

static void sha_wait_idle()
{
    while(*REG_SHA_CNT & 1);
}

static void sha(void *res, const void *src, u32 size, u32 mode)
{
    sha_wait_idle();
    *REG_SHA_CNT = mode | SHA_CNT_OUTPUT_ENDIAN | SHA_NORMAL_ROUND;
    
    const u32 *src32 = (const u32 *)src;
    int i;
    while(size >= 0x40)
    {
        sha_wait_idle();
        for(i = 0; i < 4; ++i)
        {
            *REG_SHA_INFIFO = *src32++;
            *REG_SHA_INFIFO = *src32++;
            *REG_SHA_INFIFO = *src32++;
            *REG_SHA_INFIFO = *src32++;
        }

        size -= 0x40;
    }
    
    sha_wait_idle();
    memcpy((void *)REG_SHA_INFIFO, src32, size);
    
    *REG_SHA_CNT = (*REG_SHA_CNT & ~SHA_NORMAL_ROUND) | SHA_FINAL_ROUND;
    
    while(*REG_SHA_CNT & SHA_FINAL_ROUND);
    sha_wait_idle();
    
    u32 hashSize = SHA_256_HASH_SIZE;
    if(mode == SHA_224_MODE)
        hashSize = SHA_224_HASH_SIZE;
    else if(mode == SHA_1_MODE)
        hashSize = SHA_1_HASH_SIZE;

    memcpy(res, (void *)REG_SHA_HASH, hashSize);
}

/*****************************************************************/

static u8 __attribute__((aligned(4))) nandCtr[AES_BLOCK_SIZE];
static u8 nandSlot;
static u32 fatStart;

void ctrNandInit(void)
{
    u8 __attribute__((aligned(4))) cid[AES_BLOCK_SIZE];
    u8 __attribute__((aligned(4))) shaSum[SHA_256_HASH_SIZE];

    sdmmc_get_cid(1, (u32 *)cid);
    sha(shaSum, cid, sizeof(cid), SHA_256_MODE);
    memcpy(nandCtr, shaSum, sizeof(nandCtr));

    if(isN3DS)
    {
        u8 __attribute__((aligned(4))) keyY0x5[AES_BLOCK_SIZE] = {0x4D, 0x80, 0x4F, 0x4E, 0x99, 0x90, 0x19, 0x46, 0x13, 0xA2, 0x04, 0xAC, 0x58, 0x44, 0x60, 0xBE};
        aes_setkey(0x05, keyY0x5, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);

        nandSlot = 0x05;
        fatStart = 0x5CAD7;
    }
    else
    {
        nandSlot = 0x04;
        fatStart = 0x5CAE5;
    }
}

u32 ctrNandRead(u32 sector, u32 sectorCount, u8 *outbuf)
{
    u8 __attribute__((aligned(4))) tmpCtr[sizeof(nandCtr)];
    memcpy(tmpCtr, nandCtr, sizeof(nandCtr));
    aes_advctr(tmpCtr, ((sector + fatStart) * 0x200) / AES_BLOCK_SIZE, AES_INPUT_BE | AES_INPUT_NORMAL);

    //Read
    u32 result;
    if(firmSource == FIRMWARE_SYSNAND)
        result = sdmmc_nand_readsectors(sector + fatStart, sectorCount, outbuf);
    else
    {
        sector += emuOffset;
        result = sdmmc_sdcard_readsectors(sector + fatStart, sectorCount, outbuf);
    }

    //Decrypt
    aes_use_keyslot(nandSlot);
    aes(outbuf, outbuf, sectorCount * 0x200 / AES_BLOCK_SIZE, tmpCtr, AES_CTR_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);

    return result;
}

void set6x7xKeys(void)
{
    if(!isDevUnit)
    {
        const u8 __attribute__((aligned(4))) keyX0x25[AES_BLOCK_SIZE] = {0xCE, 0xE7, 0xD8, 0xAB, 0x30, 0xC0, 0x0D, 0xAE, 0x85, 0x0E, 0xF5, 0xE3, 0x82, 0xAC, 0x5A, 0xF3};
        const u8 __attribute__((aligned(4))) keyY0x2F[AES_BLOCK_SIZE] = {0xC3, 0x69, 0xBA, 0xA2, 0x1E, 0x18, 0x8A, 0x88, 0xA9, 0xAA, 0x94, 0xE5, 0x50, 0x6A, 0x9F, 0x16};

        aes_setkey(0x25, keyX0x25, AES_KEYX, AES_INPUT_BE | AES_INPUT_NORMAL);
        aes_setkey(0x2F, keyY0x2F, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);

        /* [3dbrew] The first 0x10-bytes are checked by the v6.0/v7.0 NATIVE_FIRM keyinit function, 
                    when non-zero it clears this block and continues to do the key generation.
                    Otherwise when this block was already all-zero, it immediately returns. */
        memset32((void *)0x01FFCD00, 0, 0x10);
    }
}

void decryptExeFs(u8 *inbuf)
{
    u8 *exeFsOffset = inbuf + *(u32 *)(inbuf + 0x1A0) * 0x200;
    u32 exeFsSize = *(u32 *)(inbuf + 0x1A4) * 0x200;
    u8 __attribute__((aligned(4))) ncchCtr[AES_BLOCK_SIZE] = {0};

    for(u32 i = 0; i < 8; i++)
        ncchCtr[7 - i] = *(inbuf + 0x108 + i);
    ncchCtr[8] = 2;

    aes_setkey(0x2C, inbuf, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_use_keyslot(0x2C);
    aes(inbuf - 0x200, exeFsOffset, exeFsSize / AES_BLOCK_SIZE, ncchCtr, AES_CTR_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);
}

void decryptNusFirm(const u8 *inbuf, u8 *outbuf, u32 ncchSize)
{
    const u8 keyY0x3D[AES_BLOCK_SIZE] = {0x0C, 0x76, 0x72, 0x30, 0xF0, 0x99, 0x8F, 0x1C, 0x46, 0x82, 0x82, 0x02, 0xFA, 0xAC, 0xBE, 0x4C};
    u8 __attribute__((aligned(4))) cetkIv[AES_BLOCK_SIZE] = {0};
    u8 __attribute__((aligned(4))) titleKey[AES_BLOCK_SIZE];
    memcpy(titleKey, inbuf + 0x1BF, sizeof(titleKey));
    memcpy(cetkIv, inbuf + 0x1DC, 8);

    aes_setkey(0x3D, keyY0x3D, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_use_keyslot(0x3D);
    aes(titleKey, titleKey, 1, cetkIv, AES_CBC_DECRYPT_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);

    u8 __attribute__((aligned(4))) ncchIv[AES_BLOCK_SIZE] = {0};

    aes_setkey(0x16, titleKey, AES_KEYNORMAL, AES_INPUT_BE | AES_INPUT_NORMAL);
    aes_use_keyslot(0x16);
    aes(outbuf, outbuf, ncchSize / AES_BLOCK_SIZE, ncchIv, AES_CBC_DECRYPT_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);

    decryptExeFs(outbuf);
}

void arm9Loader(u8 *arm9Section)
{
    //Determine the arm9loader version
    u32 a9lVersion;
    switch(arm9Section[0x53])
    {
        case 0xFF:
            a9lVersion = 0;
            break;
        case '1':
            a9lVersion = 1;
            break;
        default:
            a9lVersion = 2;
            break;
    }

    bool needToDecrypt = *(u32 *)(arm9Section + 0x800) != 0x47704770;

    if(a9lVersion == 2 || (a9lVersion == 1 && needToDecrypt))
    {
        if(!isDevUnit)
        {
            //Set 0x11 keyslot
            const u8 __attribute__((aligned(4))) key1[AES_BLOCK_SIZE] = {0x07, 0x29, 0x44, 0x38, 0xF8, 0xC9, 0x75, 0x93, 0xAA, 0x0E, 0x4A, 0xB4, 0xAE, 0x84, 0xC1, 0xD8};
            const u8 __attribute__((aligned(4))) key2[AES_BLOCK_SIZE] = {0x42, 0x3F, 0x81, 0x7A, 0x23, 0x52, 0x58, 0x31, 0x6E, 0x75, 0x8E, 0x3A, 0x39, 0x43, 0x2E, 0xD0};
            aes_setkey(0x11, a9lVersion == 2 ? key2 : key1, AES_KEYNORMAL, AES_INPUT_BE | AES_INPUT_NORMAL);
        }

        if(needToDecrypt)
        {
            //Set keyX
            u8 __attribute__((aligned(4))) keyX[AES_BLOCK_SIZE];
            aes_use_keyslot(0x11);
            aes(keyX, arm9Section + 0x60, 1, NULL, AES_ECB_DECRYPT_MODE, 0);
            aes_setkey(0x16, keyX, AES_KEYX, AES_INPUT_BE | AES_INPUT_NORMAL);
        }
    }

    if(needToDecrypt)
    {
        u8 arm9BinSlot = a9lVersion > 0 ? 0x16 : 0x15;

        //Set keyY
        u8 __attribute__((aligned(4))) keyY[AES_BLOCK_SIZE];
        memcpy(keyY, arm9Section + 0x10, sizeof(keyY));
        aes_setkey(arm9BinSlot, keyY, AES_KEYY, AES_INPUT_BE | AES_INPUT_NORMAL);

        //Set CTR
        u8 __attribute__((aligned(4))) arm9BinCtr[AES_BLOCK_SIZE];
        memcpy(arm9BinCtr, arm9Section + 0x20, sizeof(arm9BinCtr));

        //Calculate the size of the ARM9 binary
        u32 arm9BinSize = 0;
        //http://stackoverflow.com/questions/12791077/atoi-implementation-in-c
        for(u8 *tmp = arm9Section + 0x30; *tmp != 0; tmp++)
            arm9BinSize = (arm9BinSize << 3) + (arm9BinSize << 1) + *tmp - '0';

        //Decrypt arm9bin
        aes_use_keyslot(arm9BinSlot);
        aes(arm9Section + 0x800, arm9Section + 0x800, arm9BinSize / AES_BLOCK_SIZE, arm9BinCtr, AES_CTR_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);
    }

    //Set >=9.6 KeyXs
    if(a9lVersion == 2)
    {
        u8 __attribute__((aligned(4))) keyData[AES_BLOCK_SIZE] = {0xDD, 0xDA, 0xA4, 0xC6, 0x2C, 0xC4, 0x50, 0xE9, 0xDA, 0xB6, 0x9B, 0x0D, 0x9D, 0x2A, 0x21, 0x98};
        u8 __attribute__((aligned(4))) decKey[sizeof(keyData)];

        //Set keys 0x19..0x1F keyXs
        aes_use_keyslot(0x11);
        for(u8 slot = 0x19; slot < 0x20; slot++, keyData[0xF]++)
        {
            aes(decKey, keyData, 1, NULL, AES_ECB_DECRYPT_MODE, 0);
            aes_setkey(slot, decKey, AES_KEYX, AES_INPUT_BE | AES_INPUT_NORMAL);
        }
    }
}

void computePinHash(u8 *outbuf, const u8 *inbuf)
{
    u8 __attribute__((aligned(4))) cid[AES_BLOCK_SIZE];
    u8 __attribute__((aligned(4))) cipherText[AES_BLOCK_SIZE];

    sdmmc_get_cid(1, (u32 *)cid);
    aes_use_keyslot(4); //Console-unique keyslot whose keys are set by the ARM9 bootROM
    aes(cipherText, in, 1, cid, AES_CBC_ENCRYPT_MODE, AES_INPUT_BE | AES_INPUT_NORMAL);
    sha(out, cipherText, sizeof(cipherText), SHA_256_MODE);
}