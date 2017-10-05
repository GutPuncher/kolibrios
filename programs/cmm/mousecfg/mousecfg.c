// Mouse Configuration Utility ver 1.4

#ifndef AUTOBUILD
#include "lang.h--"
#endif

#define MEMSIZE 0x23E80
#include "..\lib\strings.h" 
#include "..\lib\mem.h" 
#include "..\lib\file_system.h"
#include "..\lib\gui.h"
#include "..\lib\obj\libio.h"
#include "..\lib\obj\box_lib.h"
#include "..\lib\obj\libini.h"
#include "..\lib\patterns\restart_process.h"

// Translatiions
#ifdef LANG_RUS
	?define WINDOW_TITLE "�஢�ઠ � ����ன�� ��ࠬ��஢ ���"
	?define CHECK_MOUSE_1 "������ �� �⮩ ������"
	?define CHECK_MOUSE_2 "��� �஢�ન ������ ���"
	?define POINTER_SPEED "����⥫� ᪮��� 㪠��⥫� ���"
	?define ACCELERATION_TEXT "���⢨⥫쭮��� 㪠��⥫� ���"
	?define DOUBLE_CLICK_TEXT "����প� �������� ����� �����"
	?define MOUSE_EMULATION "��ࠢ����� 㪠��⥫�� ��� �१ ����������"
	?define MADMOUSE "������� ��� ����� ��஭� ��࠭�"
#else
	?define WINDOW_TITLE "Mouse testing and configuration"
	?define CHECK_MOUSE_1 "Click on this area to"
	?define CHECK_MOUSE_2 "check your mouse buttons"
	?define POINTER_SPEED "Mouse pointer speed divider"
	?define ACCELERATION_TEXT "Mouse pointer sensitivity"
	?define DOUBLE_CLICK_TEXT "Mouse double click delay"
	?define MOUSE_EMULATION "Enable mouse emulation using keyboard NumPad"
	?define MADMOUSE "Through screen sides for pointer"
	#endif

proc_info Form;
frame mouse_frame = { 0, 000, 14, 130, 14, 0x000111, 0xFFFfff, 0, 0, 0, 0, 6, 0x000111, 0xCCCccc };
char pos_x = 22;

unsigned char panels_img_data[] = FROM "mouse_image.raw";
raw_image panels_img = { 59, 101, #panels_img_data };

_ini ini = { "/sys/settings/system.ini", "mouse" };

struct _mouse_cfg {
	char pointer_speed, 
	acceleration,
	emulation, 
	madmouse, 
	button_clicked;
	word double_click_delay;
} mouse_cfg;


void main() {
	char id, old_button_clicked;
	
	load_dll(libini, #lib_init,1);
	load_dll(boxlib, #box_lib_init,0);
	
	LoadCfg();

	SetEventMask(EVM_REDRAW+EVM_KEY+EVM_BUTTON+EVM_MOUSE+EVM_MOUSE_FILTER);	

	loop() switch(WaitEvent())
	{
		case evMouse:
				mouse.get();
				if (mouse.y <= mouse_frame.start_y) || (mouse.y >= mouse_frame.start_y + mouse_frame.size_y) 
				|| (mouse.x >= mouse_frame.start_x + mouse_frame.size_x) || (mouse.x <= mouse_frame.start_x) break;
				old_button_clicked = mouse_cfg.button_clicked;
				if (mouse.lkm) mouse_cfg.button_clicked=1;
				else if (mouse.pkm) mouse_cfg.button_clicked=2;
				else if (mouse.mkm) mouse_cfg.button_clicked=3;
				else mouse_cfg.button_clicked=0;
				if (mouse_cfg.button_clicked != old_button_clicked) DrawMouseImage();
				break;

		case evButton: 
				id=GetButtonID();
				switch (id) {
					case 1:
						ExitApp();
						break;
					case 99:
						mouse_cfg.button_clicked=0;
						DrawMouseImage();
						break;
					case 100:
						if (mouse_cfg.emulation==true) KillProcessByName("mousemul", SINGLE);
						else RunProgram("/sys/mousemul", 0);
						mouse_cfg.emulation ^= 1;
						break;
					case 101: 
						if (mouse_cfg.madmouse==true) KillProcessByName("madmouse", SINGLE);
						else RunProgram("/sys/madmouse", 0);
						mouse_cfg.madmouse ^= 1;
						break;
					case 120: mouse_cfg.pointer_speed++; break;
					case 121: if (mouse_cfg.pointer_speed>0) mouse_cfg.pointer_speed--; break;
					case 122: mouse_cfg.acceleration++; break;
					case 123: if (mouse_cfg.acceleration>0) mouse_cfg.acceleration--; break;
					case 124: mouse_cfg.double_click_delay+=8; break;
					case 125: if (mouse_cfg.double_click_delay>0) mouse_cfg.double_click_delay-=8; break;					
				}
				DrawControls();
				ApplyCfg();

				break;

		case evKey:
				GetKeys();
				if (key_scancode == SCAN_CODE_ESC) ExitApp();
				break;
			
		case evReDraw:
				system.color.get();
				DefineAndDrawWindow(430, 150, 424, 310+skin_height,0x34,system.color.work,WINDOW_TITLE,0);
				GetProcessInfo(#Form, SelfInfo);
				if (Form.status_window>2) break;
				SetFrameColors();
				DefineButton(mouse_frame.start_x+2, mouse_frame.start_y+2, mouse_frame.size_x-4, 
					mouse_frame.size_y-4, 99+BT_NOFRAME, 0xF0F2F3); //needed to handle mouse_up and refresh mouse image
				frame_draw stdcall (#mouse_frame);
				WriteText(pos_x + 110, mouse_frame.start_y + 25, 0x90, 0x2C343C, CHECK_MOUSE_1);
				WriteText(pos_x + 110, mouse_frame.start_y + 45, 0x90, 0x2C343C, CHECK_MOUSE_2);
				DrawMouseImage();
				DrawControls();
	}
}


void DrawMouseImage() {
	_PutImage(mouse_frame.start_x+30, mouse_frame.start_y + 15,  panels_img.w, panels_img.h, 
		mouse_cfg.button_clicked * panels_img.w * panels_img.h * 3 + panels_img.data);
}

void DrawControls() {
	MoreLessBox(pos_x, mouse_frame.start_y + 142, 120, 121, mouse_cfg.pointer_speed, POINTER_SPEED);
	MoreLessBox(pos_x, mouse_frame.start_y + 172, 122, 123, mouse_cfg.acceleration, ACCELERATION_TEXT);
	MoreLessBox(pos_x, mouse_frame.start_y + 202, 124, 125, mouse_cfg.double_click_delay, DOUBLE_CLICK_TEXT);
	CheckBox(pos_x, mouse_frame.start_y + 236, 100, MOUSE_EMULATION, mouse_cfg.emulation);
	CheckBox(pos_x, mouse_frame.start_y + 262, 101, MADMOUSE, mouse_cfg.madmouse);
}

void SetFrameColors() {
	mouse_frame.size_x = - mouse_frame.start_x * 2 + Form.cwidth;
	mouse_frame.font_color = system.color.work_text;
	mouse_frame.font_backgr_color = system.color.work;
	mouse_frame.ext_col = system.color.work_graph;
}

void LoadCfg() {
	mouse_cfg.acceleration = ini.GetInt("acceleration", GetMouseAcceleration());
	mouse_cfg.pointer_speed = ini.GetInt("speed", GetMouseSpeed());
	mouse_cfg.double_click_delay = ini.GetInt("double_click_delay", GetMouseDoubleClickDelay());
	mouse_cfg.madmouse = CheckProcessExists("MADMOUSE");
	mouse_cfg.emulation = CheckProcessExists("MOUSEMUL");
}

void ExitApp() {
	ini.SetInt("acceleration", mouse_cfg.acceleration);
	ini.SetInt("speed", mouse_cfg.pointer_speed);
	ini.SetInt("double_click_delay", mouse_cfg.double_click_delay);
	ExitProcess();
}

void ApplyCfg() {
	SetMouseSpeed(mouse_cfg.pointer_speed);
	SetMouseAcceleration(mouse_cfg.acceleration);
	SetMouseDoubleClickDelay(mouse_cfg.double_click_delay);
}

stop: