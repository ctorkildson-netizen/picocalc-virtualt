WELCOME TO VIRTUALT ON THE PICOCALC
===================================
This is a TRS-80 Model 100 from 1983,
running on your PicoCalc. You are
reading README.DO in TEXT, its editor.
Arrows scroll; Shift+F3 (the Model
100's F8) goes back to the menu.

PICOCALC KEYS
Ctrl+F1   help and info
Ctrl+F4   library: load programs
          from the SD card
Ctrl+Esc  save and quit to SD menu
Shift+F1-F3      F6-F8
Shift+Esc        BREAK (stop BASIC)

TO RUN A PROGRAM
Press Ctrl+F4, pick one, Enter, then
Esc. It appears on the menu: arrow to
it and press Enter. PROGRAMS.TXT on
the SD card describes them all. Good
first ones: SOL31C, ROG, LIFE, MURDER,
POKER, OTHELO, INVADR, GREED.

TYPE ONE IN
That's how most people met BASIC in
1983. From the menu pick BASIC, type
NEW and Enter, then each line below
and Enter. RUN starts it; Shift+Esc
stops it; MENU goes back.

The time:
PRINT DAY$;" ";DATE$;" ";TIME$

A scale:
10 FOR N=1 TO 8:READ P:SOUND P,8:NEXT
20 DATA 9394,8368,7456,7032
30 DATA 6269,5586,4976,4697

Lines across the screen:
10 CLS:FOR X=0 TO 239 STEP 8
20 LINE(X,0)-(239-X,63):NEXT
30 GOTO 30

Guess a number:
10 N=INT(RND(1)*100)+1:T=0
20 INPUT "Guess (1-100)";G:T=T+1
30 IF G<N THEN PRINT "Higher":GOTO 20
40 IF G>N THEN PRINT "Lower":GOTO 20
50 PRINT "Got it in";T;"tries!"

SAVE "GUESS" keeps a program; it is
then on the menu too. Memory is saved
to the SD card when you quit.

Found a bug? VIRTUALT.TXT, REPORTING
A PROBLEM, says what to send.

To remove this file: at the menu,
Ctrl+F4, Tab, pick README.DO, Del, Y.
A fresh copy comes back only on a
fresh (cold-started) machine.
