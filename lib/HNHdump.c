#include "HNHdump.h"
void dump_16(const unsigned char *buffer,int start, int finish){
    int i;
    if(finish>start + 16) finish = start + 16;
    for(i=start;i<finish;i++){
        printf("%02x ",buffer[i]);
    }
    for(i=finish;i<start + 16;i++){
        printf("   ");
    }
    printf("    ");
    for(i=start;i<finish;i++){
        if(buffer[i]>=32 && buffer[i]>=32)
            printf("%c",buffer[i]);
        else
            printf(".");
    }
    //printf("\r\n");
}
void dump(const unsigned char *buffer,int start,int finish){
    int i;
    for(i=0;i<=((finish-start)/16);i++){
        dump_16(buffer,(start+(i*16)),finish);
        printf("\r\n");
    }

}
