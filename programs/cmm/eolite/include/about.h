#define BROWSER_PATH    "/sys/network/webview"
#define BROWSER_LINK    "http://kolibri-n.org/inf/eolite/eolite_p1"


void about_dialog()
{   
	int id;
	proc_info about_form;
	int about_x;

	if (active_about) {cmd_free=2;ExitProcess();} else active_about=1;
	loop() switch(WaitEvent())
	{
		case evButton: 
				id=GetButtonID();
				IF (id==1)
				{
					active_about=0;
					cmd_free = 2;
					ExitProcess();
				}
				IF (id==11) RunProgram(BROWSER_PATH, BROWSER_LINK);
				break;
				
		case evKey:
				GetKeys();
				IF (key_scancode == SCAN_CODE_ESC)
				{
					active_about=0;
					cmd_free = 2;
					ExitProcess();
				}
				break;
				
		case evReDraw:
				DefineAndDrawWindow(Form.left+Form.width/2,Form.top+Form.height/2-114,300,300+skin_h,0x34,sc.work,T_ABOUT,0);
				GetProcessInfo(#about_form, SelfInfo);
				if (about_form.status_window>2) break;
				logo_pal[0] = sc.work;
				ESDWORD[#logo_pal+16] = sc.work_dark;
				PutPaletteImage(#logo,86,86,about_form.cwidth-86/2,10,8,#logo_pal);
				about_x = -strlen(ABOUT_TITLE)*18+about_form.cwidth/2;
				WriteTextB(about_x+2,107,0x82,0xD49CD2,ABOUT_TITLE);
				WriteTextB(about_x,105,0x82,0x9D129D,ABOUT_TITLE);
				WriteTextLines(73,163,0x90,sc.work_text,"    By Leency\nand KolibriOS Team\n    2008-2021",20);
				#ifdef LANG_RUS
				DrawStandartCaptButton(60,about_form.cheight-38,11,"����� ࠧࠡ�⪨");
				#endif
	}
}
