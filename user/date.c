/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* date: print the current UTC date/time as YYYY-MM-DD HH:MM:SS UTC. */
#include "usys.h"
static int isleap(int y){return (y%4==0&&y%100!=0)||y%400==0;}
static int p2(char* b,int v){b[0]=(char)('0'+v/10);b[1]=(char)('0'+v%10);return 2;}
int umain(void){
    struct timespec ts;
    if(uclock_gettime(CLOCK_REALTIME,&ts)<0){uwrite(2,"date: clock failed\n",19);return 1;}
    long secs=ts.tv_sec; if(secs<0)secs=0;
    long days=secs/86400, rem=secs%86400;
    int hh=(int)(rem/3600), mm=(int)((rem%3600)/60), ss=(int)(rem%60);
    int y=1970;
    for(;;){int dy=isleap(y)?366:365; if(days>=dy){days-=dy;y++;} else break;}
    int md[12]={31,isleap(y)?29:28,31,30,31,30,31,31,30,31,30,31};
    int mon=0; while(mon<12&&days>=md[mon]){days-=md[mon];mon++;}
    int day=(int)days+1;
    char b[40]; int p=0;
    b[p++]=(char)('0'+(y/1000)%10);b[p++]=(char)('0'+(y/100)%10);b[p++]=(char)('0'+(y/10)%10);b[p++]=(char)('0'+y%10);
    b[p++]='-';p+=p2(b+p,mon+1);b[p++]='-';p+=p2(b+p,day);
    b[p++]=' ';p+=p2(b+p,hh);b[p++]=':';p+=p2(b+p,mm);b[p++]=':';p+=p2(b+p,ss);
    const char* z=" UTC\n";while(*z)b[p++]=*z++;
    uwrite(1,b,(unsigned long)p);
    return 0;
}
