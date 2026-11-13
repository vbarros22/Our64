#include "gameopt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
	gameopt_parse("Super Mario 64 (USA).z64\t720p\nbad line\nX.z64\t480p\r\nStar Fox 64 (USA) (Rev 1).z64\t720p\r\n");
	assert(gameopt_lines("Super Mario 64 (USA).z64")==720);
	assert(gameopt_lines("Star Fox 64 (USA) (Rev 1).z64")==720);
	assert(gameopt_lines("X.z64")==480 && gameopt_lines("nope")==GAMEOPT_NATIVE && gameopt_lines(NULL)==GAMEOPT_NATIVE); /* Native is the default */
	assert(gameopt_set_lines("New.z64",720)==0 && gameopt_lines("New.z64")==720);
	assert(gameopt_set_lines("Super Mario 64 (USA).z64",480)==0 && gameopt_lines("Super Mario 64 (USA).z64")==480);
	assert(gameopt_set_lines("a\tb.z64",720)==-1);
	char t[512]; int n=gameopt_text(t,sizeof t); assert(n==(int)strlen(t));
	assert(!strcmp(t,"Super Mario 64 (USA).z64\t480p\nX.z64\t480p\nStar Fox 64 (USA) (Rev 1).z64\t720p\nNew.z64\t720p\n"));
	assert(gameopt_text(NULL,0)==n);
	gameopt_parse(t); assert(gameopt_lines("New.z64")==720);
	/* a game's own picture settings: nothing set until the player sets it, and cleared again */
	assert(gameopt_picture("New.z64",GAMEOPT_SHAPE)==-1 && gameopt_picture("nope",GAMEOPT_MODE)==-1);
	assert(gameopt_set_picture("Only.z64",GAMEOPT_SHAPE,1)==0 && gameopt_picture("Only.z64",GAMEOPT_SHAPE)==1);
	assert(gameopt_lines("Only.z64")==GAMEOPT_NATIVE);
	assert(gameopt_set_picture("New.z64",GAMEOPT_MODE,2)==0 && gameopt_set_picture("New.z64",GAMEOPT_SPEED,0)==0);
	n=gameopt_text(t,sizeof t);
	assert(!strcmp(t,"Super Mario 64 (USA).z64\t480p\nX.z64\t480p\nStar Fox 64 (USA) (Rev 1).z64\t720p\nNew.z64\t720p\tmode=2\tspeed=0\nOnly.z64\tshape=1\n"));
	gameopt_parse(t);
	assert(gameopt_picture("New.z64",GAMEOPT_MODE)==2 && gameopt_picture("New.z64",GAMEOPT_SPEED)==0 && gameopt_picture("New.z64",GAMEOPT_SHAPE)==-1);
	assert(gameopt_set_picture("Only.z64",GAMEOPT_SHAPE,-1)==0);
	n=gameopt_text(t,sizeof t); assert(!strstr(t,"Only.z64")); /* all default again: no line */
	gameopt_renamed("New.z64","Renamed.z64");
	assert(gameopt_lines("Renamed.z64")==720 && gameopt_lines("New.z64")==GAMEOPT_NATIVE);
	gameopt_parse("Bad.z64\tmode=9\tspeed=x\n"); assert(gameopt_picture("Bad.z64",GAMEOPT_MODE)==-1);
	/* native is the default (fork, 2026-11-13): an old "native" line reads as
	 * no line, 480p is written when chosen, and native again clears it */
	gameopt_parse("Lambo.z64\tnative\n");
	assert(gameopt_lines("Lambo.z64")==GAMEOPT_NATIVE);
	n=gameopt_text(t,sizeof t); assert(n==0 && !t[0]);
	assert(gameopt_set_lines("F1.z64",GAMEOPT_NATIVE)==0 && gameopt_lines("F1.z64")==GAMEOPT_NATIVE);
	n=gameopt_text(t,sizeof t); assert(n==0);
	assert(gameopt_set_lines("F1.z64",480)==0 && gameopt_lines("F1.z64")==480);
	n=gameopt_text(t,sizeof t); assert(!strcmp(t,"F1.z64\t480p\n"));
	gameopt_parse(t); assert(gameopt_lines("F1.z64")==480);
	assert(gameopt_set_lines("F1.z64",999)==0 && gameopt_lines("F1.z64")==GAMEOPT_NATIVE); /* anything else is native */
	n=gameopt_text(t,sizeof t); assert(n==0);
	(void)n;
	puts("gameopt ok"); return 0; }
