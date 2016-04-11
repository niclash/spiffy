#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <spiffs.h>

#define SPI_FLASH_SEC_SIZE 4096
#define LOG_PAGE_SIZE       SPI_FLASH_SEC_SIZE/8

#define MIN_SIZE 64*1024
#define MAX_SIZE 2048*1024

u32_t g_ROMSize = MAX_SIZE;

#define FILEDIR "filesystem"
#define ROMNAME "spiff_rom.bin"
#define ROMERASE 0xFF

static spiffs fs;
static u8_t spiffs_work_buf[LOG_PAGE_SIZE*2];
static u8_t spiffs_fds[32*4];
static u8_t spiffs_cache_buf[(LOG_PAGE_SIZE+32)*4];

#ifdef DEBUG
#define S_DBG(...) do { printf("[Debug] ") ; printf(__VA_ARGS__); } while( 0 );
#else
#define S_DBG(...)
#endif

FILE *rom;

void hexdump_mem(u8_t *b, u32_t len) {
    while (len--) {
        if ((((u32_t)b)&0x1f) == 0) {
            S_DBG("\n");
        }
        S_DBG("%02x", *b++);
    }
    S_DBG("\n");
}


static s32_t my_spiffs_read(u32_t addr, u32_t size, u8_t *dst) {

    int r = 0;

    if(fseek(rom, addr,SEEK_SET)){
        printf("Unable to seek to %d",addr);
    }

    r = fread(dst,size,1,rom);

    if(r != 1) {
        printf("Unable to read - tried to get %d bytes only got %d\n",size,r);
        return SPIFFS_ERR_NOT_READABLE;
    }

    S_DBG("Read %d bytes from offset %d\n",size,addr);
//    hexdump_mem(dst,size);
    return SPIFFS_OK;
}

static s32_t my_spiffs_write(u32_t addr, u32_t size, u8_t *src) {
    int i;
    u8_t *buf = malloc(size);

    my_spiffs_read(addr,size,buf);

    S_DBG("Was: \n");
    hexdump_mem(buf,size);

    S_DBG("Add: \n");
    hexdump_mem(src,size);

    for(i=0;i<size;i++){

        buf[i] &= src[i];
    }

    S_DBG("Now:\n");
    hexdump_mem(buf,size);

    fseek(rom, addr,SEEK_SET);

    if(fwrite(buf, size, 1, rom) != 1){
        printf("Unable to write\n");
        return SPIFFS_ERR_NOT_WRITABLE;
    }
    fflush(rom);
    S_DBG("Wrote %d bytes to offset %d\n",size,addr);
    free(buf);

    return SPIFFS_OK;
}

static s32_t my_spiffs_erase(u32_t addr, u32_t size) {
    int i;

    fseek(rom, addr, SEEK_SET);

    for(i=0; i< size; i++){
        fputc(ROMERASE,rom);
    }
    fflush(rom);

    S_DBG("Erased %d bytes at offset %d\n",size,addr);

    return SPIFFS_OK;
}



void my_spiffs_mount() {
  spiffs_config cfg;

  cfg.phys_size = g_ROMSize; // use all spi flash
  cfg.phys_addr = 0; // start spiffs at start of spi flash

  cfg.phys_erase_block = SPI_FLASH_SEC_SIZE;
  cfg.log_block_size = SPI_FLASH_SEC_SIZE;
  cfg.log_page_size = LOG_PAGE_SIZE;

  cfg.hal_read_f = my_spiffs_read;
  cfg.hal_write_f = my_spiffs_write;
  cfg.hal_erase_f = my_spiffs_erase;

  int res = SPIFFS_mount(&fs,
          &cfg,
          spiffs_work_buf,
          spiffs_fds,
          sizeof(spiffs_fds),
          spiffs_cache_buf,
          sizeof(spiffs_cache_buf),
          0);
  S_DBG("mount res: %i\n", res);
}


int write_to_spiffs(char *fname, u8_t *data,int size) {

    spiffs_file fd = SPIFFS_open(&fs, fname, SPIFFS_CREAT | SPIFFS_TRUNC | SPIFFS_RDWR, 0);

    S_DBG("Opened spiffs file %s\n",fname);

    if (SPIFFS_write(&fs, fd, (u8_t *)data, size) < 0) {
        printf("Unable to write file %s - errno %i\n", fname, SPIFFS_errno(&fs));

        return 1;
    }

    SPIFFS_close(&fs, fd);
    S_DBG("Closed spiffs file %s\n",fname);
    return 0;
}

static void test_spiffs() {
    uint8_t buf[12],out[12];

    sprintf((char*)buf,"Hi there ");

    write_to_spiffs("my_file", buf, 10);

    spiffs_file fd = SPIFFS_open(&fs, "my_file", SPIFFS_RDWR, 0);
    if (SPIFFS_read(&fs, fd, (u8_t *)out, 10) < 0) S_DBG("errno %i\n", SPIFFS_errno(&fs));
    SPIFFS_close(&fs, fd);

    printf("--> %s <--\n", buf);
}

void add_file(char *localname, char* spiffs_name) {

    int sz;
    u8_t *buf;
    char *path = malloc(1024);

    FILE *fp = fopen(localname,"rb");
    if (fp == NULL){
        S_DBG("Skipping %s\n",path);
        return;
    }

    fseek(fp, 0L, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0L, SEEK_SET);

    if (sz == -1) {
        //directory
        return;
    }
    buf = malloc(sz);
    if(fread(buf,sz,1,fp) != 1) {
        printf("Unable to read file %s\n",spiffs_name);
        return;
    }
    S_DBG("%d bytes read from %s\n",sz,fname);
    write_to_spiffs(spiffs_name, buf,sz);
    printf("%s added to spiffs (%d bytes)\n",spiffs_name,sz);
    free(buf);
    fclose(fp);
}

void add_dir(char *dirname, char *spiffs_name){
    
    DIR *dir = opendir(dirname);
    if( dir != NULL ){
        struct dirent *ent;
        char localname[4096];
        char spiff[4096];
        while ((ent = readdir(dir)) != NULL) {
            strcpy(localname, dirname);
            strcat(localname, "/");
            strcat(localname, ent->d_name);
            strcpy(spiff, spiffs_name);
            strcat(spiff, "/");
            strcat(spiff, ent->d_name);
            if(ent->d_type == DT_DIR) {
                // Exclude hidden files and current/parent dir entries
                if( ent->d_name[0] != '.' ) {
                    add_dir(localname, spiff);
                }
            } else if(ent->d_type == DT_REG) {
                add_file(localname,spiff);
            }
        } 
    } else {
        printf("Unable to open directory \"%s\"\n", dirname );
    }
    closedir(dir);
}

int main(int argc, char **args) {

	if(argc > 1)
	{
		g_ROMSize = atoi(args[1]) * 1024;
		g_ROMSize &= 0xFFFFF000; //align to 4K
		
		if( MIN_SIZE > g_ROMSize)
		{
			g_ROMSize = MIN_SIZE;
			printf("Using minimum flash size %d kB\n", g_ROMSize/1024);
		}
		else if(MAX_SIZE < g_ROMSize)
		{
			g_ROMSize = MAX_SIZE;
			printf("Using maximum flash size %d kB\n", g_ROMSize/1024);
		}
	}

    rom = fopen(ROMNAME,"w+");
    int i;
    for(i=0; i < g_ROMSize; i++) {
        fputc(ROMERASE,rom);
    }
    fflush(rom);

    my_spiffs_mount();
    printf("Creating rom %s of size %d kB\n", ROMNAME, g_ROMSize/1024);
	printf("For another rom size use \"spiffy <size_kBytes>\"\n");
    printf("Adding files in directory \"%s\"\n", FILEDIR);
    add_dir(FILEDIR, "");
    fclose(rom);
    exit(EXIT_SUCCESS);
}
