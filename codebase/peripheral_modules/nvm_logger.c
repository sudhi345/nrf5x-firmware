/* 
 * File:   nvm_logger.c
 * Copyright (c) 2018 Appiko
 * Created on 14 February, 2019, 9:10 AM
 * Author:  Tejas Vasekar (https://github.com/tejas-tj)
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE
 */

#include <math.h>

#include "nvm_logger.h"
#include "hal_nvmc.h"
#include "stdbool.h"
#include "string.h"
#include "nrf_util.h"
#include "log.h"
#include "common_util.h"

#define PAGE_METADATA_OFFSET 1

#define MEM_RESET_VALUE 0xFFFFFFFF

#define BYTE_RESET_VALUE 0xFF

#define WORD_SIZE 4

#define BYTES_PER_PAGE 4080

#define PAGE_METADATA_LOC 0xFEF

#define IN_PAGE_LOC(x)  (x && 0xFFF)

typedef enum
{
    MEMORY_AVAIL = 0x00000000,
    MEMORY_FULL = 0xFFFFFFFF,
}page_mem_availiable_t;

/** Structure to store metadata of all the logs */
typedef struct
{
    /** Size of each data entry in words */
    uint8_t entry_size;
    /** Address of first page of log */
    uint32_t page_addrs[NVM_LOGGER_MAX_PAGES];
    /** Number of pages required */
    uint8_t no_pages;
    /** Current page number */
    uint8_t current_page;
    /** Current location */
    uint32_t current_loc;
    /** Last Entry on page */
    uint32_t last_entry_no;
    /** Current entry number o page */
    uint32_t current_entry_no;
    /** Toatl no of entries present in log */
    uint32_t total_entries;
    /** size in bytes */
    uint32_t size_bytes;
}log_metadata_t;

/*
 * @brief Struct to store Page Metadata 
 */
typedef struct
{
    /** 1Byte : LOG_ID */
    uint8_t log_id;
    /** 1Byte : log_page_no */
    uint8_t log_page_no;
    /** 1Byte : data_size */
    uint16_t data_size;
    /** 4Byte : Page full flag */
    uint32_t is_mem_available;
    /** 4Byte : next page addr(NULL if last page) */
    uint32_t next_page_addr;
}__attribute__ ((packed)) page_metadata_t;

/** Number of log pages currently available to use */
static uint32_t no_avail_pages = NVM_LOG_MAX_PAGES;

static log_metadata_t LOGS[NVM_LOGGER_MAX_LOGS];

static bool avail_pages[NVM_LOG_MAX_PAGES];

static uint32_t p_empty_page[1024]; 

const log_metadata_t EMPTY_LOG_METADATA = 
{
    .current_loc = 0,
    .current_page = 0,
    .entry_size = 0,
    .last_entry_no = 0,
    .no_pages = 0,
    .total_entries = 0,
    .current_entry_no =0,
};


const page_metadata_t EMPTY_PAGE_METADATA = 
{
    .log_id = 0xFF,
    .log_page_no = 0xFF,
    .next_page_addr = 0xFFFFFFFF,
    .data_size = 0xFFFF,
    .is_mem_available = 0xFFFFFFFF
};

void prepare_page_metadata (uint32_t log_id);

void prepare_log_metadata (uint32_t * p_mem_loc, uint32_t page_no);

void empty_page (uint32_t page_addr);

void get_total_entries (uint32_t log_id)
{
    for(uint32_t page_no = 0; page_no < LOGS[log_id].no_pages; page_no++)
    {   
        for(uint32_t loc = 0; loc < LOGS[log_id].last_entry_no; loc++)
        {
            if(memcmp(
               (uint32_t *)(LOGS[log_id].page_addrs[page_no]) + LOGS[log_id].entry_size*loc,
               p_empty_page, sizeof(uint32_t) * LOGS[log_id].entry_size) != 0)
            {
//                log_printf();
                LOGS[log_id].total_entries++;
            }
        }
        log_printf("Total Entries LOGS[%d] : %d\n", log_id,LOGS[log_id].total_entries);
    }
}

uint32_t get_next_loc (uint32_t log_id)
{
    
    /* This is a dumb function which just asssumes that log exist and there is
     * empty location available. So write log_write function to take care of
     * erasing of next page */
    log_printf("%s\n",__func__);
    uint32_t page_no = 0;
    uint32_t * p_mem_loc = (uint32_t *)LOGS[log_id].page_addrs[page_no];
    bool next_loc_found = false;
    uint32_t current_page_entry_no = 0;
    while (next_loc_found == false)
    {
        if(memcmp(p_mem_loc, p_empty_page, sizeof(uint32_t) * LOGS[log_id].entry_size) == 0)
        {
            next_loc_found = true;
            LOGS[log_id].current_loc = (uint32_t)(p_mem_loc );
            LOGS[log_id].current_page = page_no;
            LOGS[log_id].current_entry_no = current_page_entry_no;
        }
        else 
        {
//            log_printf("nxt loc %x\n", p_mem_loc);
            p_mem_loc += LOGS[log_id].entry_size;
            current_page_entry_no++;
        }
        if(current_page_entry_no >= LOGS[log_id].last_entry_no)
        {
            page_no = (page_no + 1)%LOGS[log_id].no_pages;
            p_mem_loc = (uint32_t *) LOGS[log_id].page_addrs[page_no];
            current_page_entry_no = 0;
        }
    }
    log_printf("Next loc : %x\n", p_mem_loc);
    return (uint32_t)p_mem_loc;
}

uint32_t update_log (log_config_t * log_config)
{
    uint32_t cnt = 0; bool empty_log_found = false;
    while(cnt < NVM_LOGGER_MAX_LOGS && empty_log_found == false)
    {
        if (memcmp (&LOGS[log_config->log_id], &EMPTY_LOG_METADATA, sizeof(log_metadata_t)) == 0)
        {
            empty_log_found = true;
            LOGS[log_config->log_id].current_loc = log_config->start_page;
            LOGS[log_config->log_id].current_page = 0;
            LOGS[log_config->log_id].size_bytes = (log_config->entry_size );
            LOGS[log_config->log_id].entry_size = CEIL_DIV(log_config->entry_size,4);
            log_printf("Entry size init %d\n", LOGS[log_config->log_id].entry_size);
            LOGS[log_config->log_id].no_pages = log_config->no_of_pages;
            for(uint32_t page_no; page_no < log_config->no_of_pages; page_no++)
            {
                LOGS[log_config->log_id].page_addrs[page_no] = log_config->start_page 
                                    - page_no*NVM_LOGGER_PAGE_OFFSETS;
            }
            LOGS[log_config->log_id].last_entry_no = (BYTES_PER_PAGE/(LOGS[log_config->log_id].entry_size * WORD_SIZE));
        }
        else
        {
            log_config->log_id = (log_config->log_id + 1) % NVM_LOGGER_MAX_LOGS;
            cnt++;
        }
    }
    prepare_page_metadata (log_config->log_id);
    return log_config->log_id;

}

void empty_page (uint32_t page_loc)
{
    page_metadata_t page_metadata_buffer;
    page_metadata_t * page_metadata_loc = (page_metadata_t *) (page_loc + NVM_LOGGER_PAGE_METADATA_ADDR); 
    memcpy(&page_metadata_buffer, page_metadata_loc, sizeof(page_metadata_t));
    hal_nvmc_erase_page (page_loc);
    page_metadata_buffer.is_mem_available = MEMORY_AVAIL;
    hal_nvmc_write_data (page_metadata_loc, &page_metadata_buffer, sizeof(page_metadata_t));

}

void prepare_page_metadata (uint32_t log_id)
{
    page_metadata_t local_page_metadata;
    for(uint32_t page_no = 0; page_no < LOGS[log_id].no_pages; page_no++)
    {
        log_printf("%s : %x\n",__func__, LOGS[log_id].page_addrs[page_no]);
        page_metadata_t * page_metadata_loc = (page_metadata_t *)
            (LOGS[log_id].page_addrs[page_no] + NVM_LOGGER_PAGE_METADATA_ADDR);
        local_page_metadata.log_id = log_id;
        local_page_metadata.log_page_no = page_no;
        local_page_metadata.data_size = (uint16_t)LOGS[log_id].size_bytes;
        local_page_metadata.next_page_addr = 
            LOGS[log_id].page_addrs[(page_no+1)%LOGS[log_id].no_pages];
        local_page_metadata.is_mem_available = MEMORY_AVAIL;
        hal_nvmc_write_data (page_metadata_loc, &local_page_metadata, sizeof(page_metadata_t));
    }
}

void prepare_log_metadata (uint32_t * p_mem_loc, uint32_t page_no)
{
//Read page_metadata and generate the log_metadata
    log_printf("%s\n",__func__);
    page_metadata_t * local_ptr = (page_metadata_t *) p_mem_loc;
    if(memcmp (local_ptr, &EMPTY_PAGE_METADATA, sizeof(page_metadata_t)) == 0 )
    {   
        return;
    }
    LOGS[local_ptr->log_id].size_bytes = (uint32_t)local_ptr->data_size;
    LOGS[local_ptr->log_id].entry_size = CEIL_DIV(local_ptr->data_size,4);
    LOGS[local_ptr->log_id].page_addrs[local_ptr->log_page_no] = 
             ((uint32_t)p_mem_loc - NVM_LOGGER_PAGE_METADATA_ADDR);
    log_printf("Entry Size : %d, Page Addr : %x\n",LOGS[local_ptr->log_id].entry_size,LOGS[local_ptr->log_id].page_addrs[local_ptr->log_page_no]);
    LOGS[local_ptr->log_id].no_pages++;
    LOGS[local_ptr->log_id].current_loc = 0;
    LOGS[local_ptr->log_id].current_page = 0;
    LOGS[local_ptr->log_id].last_entry_no = (BYTES_PER_PAGE/(LOGS[local_ptr->log_id].entry_size*4)) ;
    log_printf("Log[%d] Last Entry : %d\n", local_ptr->log_id, LOGS[local_ptr->log_id].last_entry_no);
    avail_pages[page_no] = 0;
    no_avail_pages--;
    
}

void nvm_logger_mod_init (void)
{
    log_printf("%s\n", __func__);
    memset (p_empty_page, BYTE_RESET_VALUE, sizeof(p_empty_page));
    no_avail_pages = NVM_LOGGER_MAX_PAGES;
    uint32_t * p_mem_loc = (uint32_t *)(NVM_LOG_PAGE0 + NVM_LOGGER_PAGE_METADATA_ADDR);
    for (uint32_t page_no = 0; page_no < NVM_LOG_MAX_PAGES; page_no++)
    {
        avail_pages[page_no] = 1;
        //go to every page_metadata location
        prepare_log_metadata (p_mem_loc,page_no);
        p_mem_loc -= NVM_LOGGER_PAGE_OFFSETS/WORD_SIZE;
    }
    for(uint32_t log_no = 0; log_no < NVM_LOGGER_MAX_LOGS; log_no++)
    {
        get_next_loc (log_no);
    }
}



//setup logic
uint32_t nvm_logger_log_init (log_config_t * log_config)
{
    log_printf("%s\n", __func__);
    if(no_avail_pages == 0)
    {
        log_printf("Memory Full..!!\n");
        return NVM_LOGGER_MAX_LOGS;
    }
    else if((LOGS[log_config->log_id].size_bytes == log_config->entry_size) && 
       (LOGS[log_config->log_id].no_pages == log_config->no_of_pages) &&
       (LOGS[log_config->log_id].page_addrs[0] == log_config->start_page))
        
    {
        log_printf("Log already present..!!\n");
        get_total_entries (log_config->log_id);
        return log_config->log_id;
    }
    else if(no_avail_pages >= log_config->no_of_pages) 
        
    {
        log_printf("New Log..!!\n");
        no_avail_pages -= log_config->no_of_pages;
        return update_log (log_config);
    }
    else
    {
        log_printf("Not enough Pages available..!!\n");
        return NVM_LOGGER_MAX_LOGS;
    }    
    return -1;
    
}


//Writing logic
void nvm_logger_feed_data (uint32_t log_id, void * data)
{
    uint32_t * p_buff = (uint32_t *)LOGS[log_id].current_loc;

    hal_nvmc_write_data (p_buff, (uint8_t *)data,
                             LOGS[log_id].size_bytes);
    {
        LOGS[log_id].current_entry_no ++;
        LOGS[log_id].current_loc += LOGS[log_id].entry_size * WORD_SIZE;
        LOGS[log_id].total_entries++;
    }
    if(LOGS[log_id].current_entry_no < LOGS[log_id].last_entry_no )
    {
        return;
    }
    {
        log_printf("page change..!!\n");
        LOGS[log_id].current_page = ((LOGS[log_id].current_page + 1) % LOGS[log_id].no_pages);
        
        if(memcmp((uint32_t *)LOGS[log_id].page_addrs[LOGS[log_id].current_page],
            (uint32_t *)p_empty_page, BYTES_PER_PAGE) != 0)
        {
            log_printf("Erase page\n");
            LOGS[log_id].total_entries -= LOGS[log_id].last_entry_no;
            empty_page (LOGS[log_id].page_addrs[LOGS[log_id].current_page]);
        }
        
        
        LOGS[log_id].current_loc = LOGS[log_id].page_addrs[LOGS[log_id].current_page];
        LOGS[log_id].current_entry_no = 0;
    }
}

void reverse_cpy (uint32_t * p_dest, uint32_t * p_src, uint32_t no_of_bytes)
{
    uint32_t word_being_copid = 0;
    while(word_being_copid < (no_of_bytes/WORD_SIZE))
    {
        *p_dest = *p_src;
        p_dest++;
        p_src++;
        word_being_copid++;
    }
}

/*
//Getting n data.
void nvm_logger_get_n_data (uint32_t log_id, void * dest_loc, uint32_t n)
{
    log_printf("%s ",__func__);
    uint32_t bytes_to_copy = LOGS[log_id].size_bytes;
    n = get_data_validate_n (log_id,n);
//        log_printf("%d, %d\n", n,LOGS[log_id].current_entry_no);
    if (n <= LOGS[log_id].current_entry_no)
    {
        
        uint32_t * p_src = (uint32_t *)(LOGS[log_id].current_loc) - LOGS[log_id].entry_size;
        uint8_t * p_dst = (uint8_t *) dest_loc;
        while(n)
        {
        log_printf("%d\n", n);
//            log_printf(" n : %d, total entries : %d, current entry :%d\n", n, LOGS[log_id].total_entries,
//                       LOGS[log_id].current_entry_no);
            log_printf(" %x, %d\n", p_src, (uint8_t)*p_src);
//            log_printf("Bytes to copy :%d \n",  bytes_to_copy);
//            reverse_cpy ( p_dst, p_src, bytes_to_copy);
            memcpy ( p_dst, p_src, bytes_to_copy);
            n--;
            p_dst += bytes_to_copy;
            p_src -=  LOGS[log_id].entry_size;
        }
    }
    else if (n > LOGS[log_id].current_entry_no)
    {
        log_printf("In sec if\n");
        uint32_t prev_page;
        if(LOGS[log_id].current_page == 0)
        {
            prev_page = LOGS[log_id].no_pages - 1;
        }
        else
        {
            prev_page = LOGS[log_id].current_page - 1;
        }
        log_printf("Prev Page : %d\n", prev_page);
        log_printf("Entry size : %d\n", LOGS[log_id].entry_size);
        uint32_t * p_src = (uint32_t *)(LOGS[log_id].current_loc - LOGS[log_id].entry_size);
        uint32_t * p_dst = (uint32_t *) dest_loc;
        log_printf(" %x\n", p_src);
        uint32_t entry_remain = 0;
        log_printf("Bytes to copy : %d\n", bytes_to_copy);
        while(entry_remain < LOGS[log_id].current_entry_no)
        {
            reverse_cpy (p_dst, p_src, bytes_to_copy);
            p_src -= bytes_to_copy/WORD_SIZE;
            p_dst += bytes_to_copy/WORD_SIZE;
            entry_remain++;
        }

        p_src = (uint32_t *)(LOGS[log_id].page_addrs[prev_page] +
            (LOGS[log_id].entry_size * (LOGS[log_id].last_entry_no - 1)));
        p_dst = (uint32_t *)dest_loc + (LOGS[log_id].current_entry_no * LOGS[log_id].entry_size)/WORD_SIZE;
        log_printf("Bytes to copy : %d\n", bytes_to_copy);
        while(entry_remain < (LOGS[log_id].total_entries -LOGS[log_id].current_entry_no))
        {
            reverse_cpy (p_dst, p_src, bytes_to_copy);
            p_src -= bytes_to_copy/WORD_SIZE;
            p_dst += bytes_to_copy/WORD_SIZE;
            entry_remain++;
        }
    }
}
 */

void nvm_logger_fetch_tail_data (uint32_t log_id, void * dest_loc, uint32_t entry_no)
{
    uint32_t * p_dest = (uint32_t *)dest_loc;
    uint32_t * p_src = NULL;
    if(entry_no >= LOGS[log_id].total_entries)
    {
        uint32_t loc = ((LOGS[log_id].current_page) 
            + 1*(LOGS[log_id].total_entries != LOGS[log_id].current_entry_no)) 
            %LOGS[log_id].no_pages;
        p_src = (uint32_t *)(LOGS[log_id].page_addrs[loc]);
        while(memcmp(p_src,  p_empty_page, LOGS[log_id].entry_size * WORD_SIZE) == 0)
        {
            loc = (loc + 1)%LOGS[log_id].no_pages;
            p_src = (uint32_t *)(LOGS[log_id].page_addrs[loc]);
        }
        memcpy(p_dest, p_src, LOGS[log_id].size_bytes);
        return;
    }
    if(entry_no >= LOGS[log_id].current_entry_no)
    {
        entry_no = (entry_no - LOGS[log_id].current_entry_no );
        
        uint32_t entry_page = (LOGS[log_id].current_page + 
            LOGS[log_id].no_pages -1 + (entry_no/(LOGS[log_id].last_entry_no )
            *(LOGS[log_id].current_loc %1000 == 0))) % 
            LOGS[log_id].no_pages;
        entry_no = 
            (((( LOGS[log_id].current_page - entry_page + LOGS[log_id].no_pages)
            %LOGS[log_id].no_pages) * (LOGS[log_id].last_entry_no )) - entry_no *
            1);  
        p_src = (uint32_t *)(LOGS[log_id].page_addrs[entry_page]) +
            LOGS[log_id].entry_size*(entry_no);
        memcpy(p_dest, p_src, LOGS[log_id].size_bytes);
        return;
 
    }
    p_src = (uint32_t *)(LOGS[log_id].current_loc) - 
        LOGS[log_id].entry_size * (entry_no) ;
    memcpy(p_dest, p_src, LOGS[log_id].size_bytes);
    return;
    
}

//set direction flag
//


void nvm_logger_empty_log (uint32_t log_id)
{
    for(uint32_t page_no = 0; page_no < LOGS[log_id].no_pages; page_no++)
    {
        empty_page (LOGS[log_id].page_addrs[page_no]);
    }
    LOGS[log_id].current_entry_no = 0;
    LOGS[log_id].current_page = 0;
    LOGS[log_id].current_loc = LOGS[log_id].page_addrs[0];
}

bool nvm_logger_is_log_empty (uint32_t log_id)
{
    bool log_empty = true;
    for(uint32_t page_no = 0; page_no < LOGS[log_id].no_pages; page_no++)
    {
        if(memcmp((uint8_t * )LOGS[log_id].page_addrs[page_no],
                  p_empty_page, NVM_LOGGER_PAGE_METADATA_ADDR) != 0)
        {
            log_empty = false;
        }
    }
    return log_empty;
}

void nvm_logger_release_log (uint32_t log_id)
{
    for(uint32_t page_no; page_no < LOGS[log_id].no_pages; page_no++)
    {
        hal_nvmc_erase_page (LOGS[log_id].page_addrs[page_no]);
    }
}


