// GT_HelloWorldWin32.cpp
// compile with: /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS /c

#include <windows.h>
#include <stdlib.h>
#include <string>
#include <tchar.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "main.h"

#define CELL_SIZE 35


// Global variables
static team White;
static team Black;
static ball Ball;
static int invitation =0;
static int ball_posession =-10;
static int passed;
static float match_time = 0;
static TCHAR TeamName[2][6]={"White","Black"};
static int selected=-1;
static int take_back = 0;
static player* ppointers[8];
static BOOL IsInit=0;
static int team_to_move=0;
static CHAR fullmessage[55];
static CHAR initmessage[] = _T("Initialize team ");
static CHAR movemessage[] = _T("Move team ");

// The main window class name.
static TCHAR szWindowClass[] = _T("win32app");

// The string that appears in the application's title bar.
static TCHAR szTitle[] = _T("Win32 Guided Tour Application");

HINSTANCE hInst;

// uniform random index in [0, n)
static int RandIndex(int n)
{
	if (n <= 0) return 0;
	int r = (int)((double)rand() / ((double)RAND_MAX + 1.0) * n);
	return (r < n) ? r : n - 1;
}

// The ball never leaves the field: a step that would cross a field line ends on the line,
// and once the ball is on the line the velocity component perpendicular to it is zeroed.
// (x, y) - position after the step, (vx, vy) - velocity of this step; returns the clamped position
static void StopAtLines(int& x, int& y, int vx, int vy, BOOL& stopX, BOOL& stopY)
{
	stopX = FALSE; stopY = FALSE;
	if (x <= 0 && vx < 0)  { x = 0;  stopX = TRUE; }
	if (x >= 10 && vx > 0) { x = 10; stopX = TRUE; }
	if (y <= 1 && vy < 0)  { y = 1;  stopY = TRUE; }
	if (y >= 19 && vy > 0) { y = 19; stopY = TRUE; }
}

// cell is inside the playing field
static BOOL InField(int x, int y)
{
	return (x >= 0) && (x <= 10) && (y >= 1) && (y <= 19);
}

// index of the player standing in cell (x, y), or -1
static int PlayerAt(int x, int y)
{
	for (int k=0; k<8; k++)
		if ((ppointers[k]->xNew==x)&&(ppointers[k]->yNew==y)) return k;
	return -1;
}

// last touch of the ball: Ball.last_touch is the team, last_pass - it was a deliberate pass of that team
static BOOL last_pass = FALSE;
static void Touch(int team, BOOL pass)
{
	Ball.last_touch = team;
	last_pass = pass;
}

// goal mouth cells: x = 4..6 on the end lines; row 1 is in front of Black's goal (y=0), row 19 - White's (y=20)
static BOOL InMouth(int x, int y)
{
	return (x>=4)&&(x<=6)&&((y==1)||(y==19));
}

// The ball step (x0,y0)->(x1,y1) brings a free ball into a goal mouth cell where no field player stands:
// it either ends there or runs along the end line across the mouth. Returns the first such cell in (mx,my).
static BOOL FindMouthCell(int x0, int y0, int x1, int y1, int& mx, int& my)
{
	if ((y0==y1)&&(x0!=x1)&&((y1==1)||(y1==19))) // along an end line
	{
		int step = (x1>x0) ? 1 : -1;
		for (int x=x0+step; x!=x1+step; x+=step)
			if (InMouth(x,y1)&&(PlayerAt(x,y1)<0)) { mx=x; my=y1; return TRUE; }
		return FALSE;
	}
	if (InMouth(x1,y1)&&(PlayerAt(x1,y1)<0)) { mx=x1; my=y1; return TRUE; }
	return FALSE;
}

// team that restarts from the centre after the keeper has taken the ball (-1 - none), and whether it was
// a deliberate pass to the own keeper (then the opponent restarts)
static int keeper_restart = -1;
static BOOL keeper_backpass = FALSE;

// a free ball in goal mouth cell (x,y) is taken by the keeper
static void KeeperCheck(int x, int y)
{
	if ((keeper_restart>=0)||!InMouth(x,y)||(PlayerAt(x,y)>=0)) return;
	int team = (y==1) ? 1 : 0; // the keeper's team
	keeper_backpass = last_pass && (Ball.last_touch==team);
	keeper_restart = keeper_backpass ? (team^1) : team;
}

// If the keeper has taken the ball: no score change, the field is reset and the restarting team
// lines up first (and so starts from the centre).
static BOOL KeeperTakesBall(HWND hWnd, HDC hdc)
{
	if (keeper_restart < 0) return FALSE;
	int team = keeper_restart;
	if (keeper_backpass) MessageBox(hWnd,"Pass to the own keeper - possession goes to the opponent","Status",0);
	else MessageBox(hWnd,"The ball rolled to the goal - the keeper takes it","Status",0);
	ResetField(); // also clears keeper_restart
	team_to_move = team;
	DrawField(hdc);
	strcpy_s(fullmessage,initmessage);
	strcat_s(fullmessage,TeamName[team_to_move]);
	MessageBox(hWnd,fullmessage,"Advise",0);
	return TRUE;
}

// Would a ball sent from (x,y) with velocity (vx,vy) roll into the goal mouth in row 'row'
// (ignoring interceptions)? Same kinematics as the real flight.
static BOOL RollsIntoMouth(int x, int y, int vx, int vy, int row)
{
	int decx = (vx<0)-(vx>0), decy = (vy<0)-(vy>0);
	BOOL stopX, stopY;
	int mx, my;
	while (vx || vy)
	{
		int nx = x+vx, ny = y+vy;
		StopAtLines(nx, ny, vx, vy, stopX, stopY);
		if (FindMouthCell(x, y, nx, ny, mx, my)) return (my==row);
		x = nx;
		y = ny;
		if (stopX) vx = 0; else if (vx) vx += decx;
		if (stopY) vy = 0; else if (vy) vy += decy;
	}
	return FALSE;
}

// What player 'sel' of the moving team can attack: 0 - nothing, 1 - tackle the ball holder,
// 2 - poke a free ball out of a corridor closed by two opponents
static int AttackKind(int sel)
{
	if ((sel<0)||(IsInit<2)||(sel/4!=team_to_move)||(ppointers[sel]->attacked)) return 0;
	player* a = ppointers[sel];
	int bx, by;
	if (ball_posession>=0)
	{
		if (ball_posession/4==team_to_move) return 0;
		bx = ppointers[ball_posession]->xNew;
		by = ppointers[ball_posession]->yNew;
	}
	else
	{
		bx = Ball.x;
		by = Ball.y;
	}
	int dx = bx-a->xNew, dy = by-a->yNew;
	if ((abs(dx)>1)||(abs(dy)>1)||((dx==0)&&(dy==0))) return 0; // only from an adjacent cell
	// not against the velocity from the previous turn, nor against the displacement made in this turn
	if (dx*(a->x-a->xPrev)+dy*(a->y-a->yPrev) < 0) return 0;
	if (dx*(a->xNew-a->x)+dy*(a->yNew-a->y) < 0) return 0;
	if (ball_posession>=0) return 1;
	// free ball: only diagonally, when both corridor cells are occupied by opponents
	if ((dx==0)||(dy==0)) return 0;
	int c1 = PlayerAt(a->xNew, by), c2 = PlayerAt(bx, a->yNew);
	if ((c1<0)||(c2<0)||(c1/4==team_to_move)||(c2/4==team_to_move)) return 0;
	return 2;
}

// history of "the ball is sealed" over the last 3 turns of each team (bit 0 - the latest turn)
static int seal_hist[2] = {0, 0};

// Is the ball sealed by 'team': can no opponent player reach the ball cell or a cell next to it?
// Obstacles are the players of 'team' only (the opponents can move their own players away);
// steps of one cell in 8 directions, a diagonal step is blocked when both corridor cells are occupied.
static BOOL BallSealedBy(int team)
{
	int bx, by;
	if (ball_posession>=0)
	{
		if (ball_posession/4!=team) return FALSE;
		bx = ppointers[ball_posession]->xNew;
		by = ppointers[ball_posession]->yNew;
	}
	else
	{
		bx = Ball.x;
		by = Ball.y;
	}
	BOOL wall[11][21] = {{0}}, seen[11][21] = {{0}};
	int qx[11*21], qy[11*21], head = 0, tail = 0;
	for (int k=0; k<8; k++)
		if (k/4==team) wall[ppointers[k]->xNew][ppointers[k]->yNew] = TRUE;
	for (int k=0; k<8; k++)
	{
		int x = ppointers[k]->xNew, y = ppointers[k]->yNew;
		if ((k/4!=team)&&!seen[x][y]) { seen[x][y] = TRUE; qx[tail] = x; qy[tail++] = y; }
	}
	while (head<tail)
	{
		int x = qx[head], y = qy[head++];
		if ((abs(x-bx)<=1)&&(abs(y-by)<=1)) return FALSE; // the ball or a neighbouring cell is reachable
		for (int dx=-1; dx<=1; dx++)
			for (int dy=-1; dy<=1; dy++)
			{
				int nx = x+dx, ny = y+dy;
				if ((dx==0&&dy==0)||!InField(nx,ny)||wall[nx][ny]||seen[nx][ny]) continue;
				if (dx&&dy&&wall[nx][y]&&wall[x][ny]) continue; // closed corridor
				seen[nx][ny] = TRUE;
				qx[tail] = nx;
				qy[tail++] = ny;
			}
	}
	return TRUE;
}

// Forward declarations of functions included in this code module:
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine,
                   int nCmdShow)
{
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style          = CS_HREDRAW | CS_VREDRAW | CS_OWNDC; // own DC: hdc stays valid for the window lifetime
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(NULL, IDI_APPLICATION);
    wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = NULL;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex))
    {
        MessageBox(NULL,
            _T("Call to RegisterClassEx failed!"),
            _T("Win32 Guided Tour"),
            MB_OK);

        return 1;
    }

    hInst = hInstance; // Store instance handle in our global variable

    // The parameters to CreateWindow explained:
    // szWindowClass: the name of the application
    // szTitle: the text that appears in the title bar
    // WS_OVERLAPPEDWINDOW: the type of window to create
    // CW_USEDEFAULT, CW_USEDEFAULT: initial position (x, y)
    // 500, 100: initial size (width, length)
    // NULL: the parent of this window
    // NULL: this application does not have a menu bar
    // hInstance: the first parameter from WinMain
    // NULL: not used in this application
    HWND hWnd = CreateWindow(
        szWindowClass,
        szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        800, 800,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (!hWnd)
    {
        MessageBox(NULL,
            _T("Call to CreateWindow failed!"),
            _T("Win32 Guided Tour"),
            MB_OK);

        return 1;
    }

    // The parameters to ShowWindow explained:
    // hWnd: the value returned from CreateWindow
    // nCmdShow: the fourth parameter from WinMain
    ShowWindow(hWnd,
        nCmdShow);
    UpdateWindow(hWnd);

    // Main message loop:
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int) msg.wParam;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static HDC hdc;
	int x,y,i;
	
	switch (message)
    {
	case WM_CREATE:
		//initializing structures
		ppointers[0] = &(White.Players[0]);
		ppointers[1] = &(White.Players[1]);
		ppointers[2] = &(White.Players[2]);
		ppointers[3] = &(White.Players[3]);
		ppointers[4] = &(Black.Players[0]);
		ppointers[5] = &(Black.Players[1]);
		ppointers[6] = &(Black.Players[2]);
		ppointers[7] = &(Black.Players[3]);
		ResetField();
		White.score = 0;
		Black.score = 0;
		team_to_move = 0;
		srand( (unsigned)time(NULL));
		//Invitation for team to move
		break;
	
    case WM_PAINT:
        if (!hdc) hdc = GetDC(hWnd);

		DrawField(hdc);
		if(invitation==0)
		{
			invitation = 1;
			strcpy_s(fullmessage,initmessage);
			strcat_s(fullmessage,TeamName[team_to_move]);
			MessageBox(hWnd,fullmessage,"Advise",0);
		}
		
		//обновляем окно

		ValidateRect(hWnd, NULL);
		     
        break;
		case WM_LBUTTONDOWN:
		x = LOWORD(lParam)/CELL_SIZE;
		y = HIWORD(lParam)/CELL_SIZE;
		if ((LOWORD(lParam)>499)&(LOWORD(lParam)<601)&(HIWORD(lParam)>499)&(HIWORD(lParam)<551)&(take_back==0)) //Take Back
		{
				//Restoring original positions
				for(i=0; i<4; i++)
				{
					
					ppointers[team_to_move*4+i]->xNew=ppointers[team_to_move*4+i]->x;
					ppointers[team_to_move*4+i]->yNew=ppointers[team_to_move*4+i]->y;
					ppointers[team_to_move*4+i]->moved=0;
					if(Ball.acquired ==1)
					{
						Ball.x = Ball.xPrev;
						Ball.y = Ball.yPrev;
						ball_posession=-10;
						Ball.acquired = 0;

					}
					if(ball_posession==team_to_move*4+i)
					{
								Ball.x=ppointers[team_to_move*4+i]->x;
								Ball.y=ppointers[team_to_move*4+i]->y;
					}
					
				}
				

		}

		if ((LOWORD(lParam)>499)&(LOWORD(lParam)<601)&(HIWORD(lParam)>599)&(HIWORD(lParam)<651)) //End Turn
		{
			if(IsInit<2)
			{
				//finalizing initialization of the team
				for(i=0; i<4; i++)
				{
					
					ppointers[team_to_move*4+i]->xPrev=ppointers[team_to_move*4+i]->xNew;
					ppointers[team_to_move*4+i]->yPrev=ppointers[team_to_move*4+i]->yNew;
					ppointers[team_to_move*4+i]->x=ppointers[team_to_move*4+i]->xNew;
					ppointers[team_to_move*4+i]->y=ppointers[team_to_move*4+i]->yNew;
					ppointers[team_to_move*4+i]->moved=0;
				}
				team_to_move=(team_to_move)?0:1;
				BallPosession();
				Ball.xPrev=Ball.x;
				Ball.yPrev=Ball.y;
				DrawField(hdc);
				if(IsInit==0)strcpy_s(fullmessage,initmessage);
				else strcpy_s(fullmessage,movemessage);
				strcat_s(fullmessage,TeamName[team_to_move]);
				MessageBox(hWnd,fullmessage,"Advise",0);
				passed =0;
				take_back=0;
				IsInit++;
			}
			
			else // real game
			{
				for(i=0; i<4; i++)
				{
					
					ppointers[team_to_move*4+i]->xPrev=ppointers[team_to_move*4+i]->x;
					ppointers[team_to_move*4+i]->yPrev=ppointers[team_to_move*4+i]->y;
					ppointers[team_to_move*4+i]->x=ppointers[team_to_move*4+i]->xNew;
					ppointers[team_to_move*4+i]->y=ppointers[team_to_move*4+i]->yNew;
					ppointers[team_to_move*4+i]->moved=0;
					ppointers[team_to_move*4+i]->attacked=0;
				}
				if(ball_posession==-10)
				{	
					if(Ball.update==0)
					{
						Intercept(hWnd, -1);
						BallPosession();
						if(ball_posession==-10)
						{
							if(Ball.vx)Ball.vx+=Ball.decx;
							if(Ball.vy)Ball.vy+=Ball.decy;
						}
					}
					Ball.update^=1;
					BallPosession();
				}
				Ball.xPrev=Ball.x;
				Ball.yPrev=Ball.y;
				// deadlock: the moving team has sealed the ball so that the opponent can not reach it
				int sealing_team = team_to_move, seal_penalty = 0, seal_warning = 0;
				if (keeper_restart<0)
				{
					int& h = seal_hist[sealing_team];
					h = ((h<<1)|(BallSealedBy(sealing_team)?1:0))&7;
					if (h&1)
					{
						if (((h&1)+((h>>1)&1)+((h>>2)&1))>=2) seal_penalty = 1; // sealed on 2 of the last 3 turns
						else seal_warning = 1;
					}
				}
				team_to_move=(team_to_move)?0:1;
				passed =0;
				take_back =0;
				selected =-1;
				Ball.acquired =0;
				match_time+=0.5;
				if (match_time==45.0)
				{
					MessageBox(hWnd,"End of first half","Status",0);
					ResetField();
					DrawField(hdc);
					team_to_move=1;
					strcpy_s(fullmessage,initmessage);
					strcat_s(fullmessage,TeamName[team_to_move]);
					MessageBox(hWnd,fullmessage,"Advise",0);
				}
				else if (KeeperTakesBall(hWnd, hdc)) {}
				else if (seal_penalty)
				{
					char seal_msg[128];
					sprintf_s(seal_msg, "%s blocked the ball - possession goes to %s", TeamName[sealing_team], TeamName[sealing_team^1]);
					MessageBox(hWnd,seal_msg,"Status",0);
					ResetField();
					team_to_move = sealing_team^1;
					DrawField(hdc);
					strcpy_s(fullmessage,initmessage);
					strcat_s(fullmessage,TeamName[team_to_move]);
					MessageBox(hWnd,fullmessage,"Advise",0);
				}
				else
				{
					DrawField(hdc);
					if (seal_warning)
					{
						char seal_msg[128];
						sprintf_s(seal_msg, "%s is blocking the ball! Open it, or possession goes to %s", TeamName[sealing_team], TeamName[sealing_team^1]);
						MessageBox(hWnd,seal_msg,"Warning",0);
					}
					strcpy_s(fullmessage,movemessage);
					strcat_s(fullmessage,TeamName[team_to_move]);
					MessageBox(hWnd,fullmessage,"Advise",0);
					if (match_time==90.0) MessageBox(hWnd,"Game over!","Status",0);
				}
			}

		}
		else if((LOWORD(lParam)>499)&(LOWORD(lParam)<601)&(HIWORD(lParam)>299)&(HIWORD(lParam)<351)) //Goal Shot
		{
			if((IsInit<2)||(selected<0)||(ball_posession!=selected)||(selected/4!=team_to_move)||(passed==2))
				MessageBox(hWnd,"This Player can not shoot!","Warning",0);
			else
			{	
				MessageBox(hWnd,"Goal Shot!","Warning",0);
				take_back = 1;
				passed = 2; // the ball has been played this turn
				int state = GoalShot(hWnd);
				if(state)
				{
					if (state==1)
					{
						if(team_to_move) Black.score++;
						else White.score++;
					}
					ResetField();
					team_to_move = team_to_move^1;
					DrawField(hdc);
					strcpy_s(fullmessage,initmessage);
					strcat_s(fullmessage,TeamName[team_to_move]);
					//strcat_s(fullmessage,tail);
					MessageBox(hWnd,fullmessage,"Advise",0);
				}
			}
		}

		else if((LOWORD(lParam)>499)&(LOWORD(lParam)<601)&(HIWORD(lParam)>199)&(HIWORD(lParam)<251)) //Pass
		{
			if(IsInit<2) MessageBox(hWnd,"No passes during the line-up","Warning",0);
			else if((ball_posession<0)||(ball_posession/4!=team_to_move)) MessageBox(hWnd,"Your team does not have the ball","Warning",0);
			else if(passed==0)
			{
				MessageBox(hWnd,"Select target for pass or shot","Advise",0);
				selected = ball_posession;
				passed=1;
			}
			else MessageBox(hWnd,"You have already moved the ball","Warning",0);
		}

		else if((LOWORD(lParam)>499)&(LOWORD(lParam)<601)&(HIWORD(lParam)>399)&(HIWORD(lParam)<451)) //Attack
		{
			int kind = AttackKind(selected);
			if (selected==-1) MessageBox(hWnd,"Attacking player is not selected","Warning",0);
			else if (kind==0)
			{
				MessageBox(hWnd,"This player can not attack!","Warning",0);
				selected =-1;
			}
			else
			{
				if (kind==1) AttackPlayer(hWnd);
				else PokeBall(hWnd);
				take_back =1;
				if ((selected>=0)&&(ppointers[selected]->moved==0)) CountDestinations(hdc); // the attack restricts his move
				if(KeeperTakesBall(hWnd, hdc)) break;
			}
		}
		else //initialization or game
		{
			if (passed==1)
			{
				//field selection
				int dist_x = (int)abs(ppointers[ball_posession]->xNew-x);
				int dist_y = (int)abs(ppointers[ball_posession]->yNew-y);
				if((!InField(x,y))|((dist_x==0)&(dist_y==0))|(dist_x>5)|(dist_y>5))
				{
					MessageBox(hWnd,"No Pass","Warning",0);
					passed = 0;
				}
				else
				{
						ball_posession=-10;
						Ball.vx=x-Ball.x;
						Ball.vy=y-Ball.y;
						Ball.decx =(Ball.vx<0)-(Ball.vx>0);
						Ball.decy =(Ball.vy<0)-(Ball.vy>0);
						passed =2;
						DrawField(hdc);
						const char* question = RollsIntoMouth(Ball.x, Ball.y, Ball.vx, Ball.vy, team_to_move ? 1 : 19)
							? "The ball will roll to your own keeper - possession will go to the opponent! Pass anyway?"
							: "Confirm action";
						int confirm = MessageBox(hWnd,question,"Are you sure?",MB_OKCANCEL);
						if(confirm==1)
						{
							passed = 2;
							take_back =1;
							Touch(team_to_move, TRUE); // deliberate pass
							Intercept(hWnd, selected);
							if(KeeperTakesBall(hWnd, hdc)) break;
							if(ball_posession==-10)BallPosession();
							if(ball_posession==-10)
							{
								if(Ball.vx)Ball.vx+=Ball.decx;
								if(Ball.vy)Ball.vy+=Ball.decy;
								Ball.update =1;
							}
						}
						else
						{
							passed = 0;
							ball_posession = selected;
							take_back =0;
						}
										
				}
				
			}

			else if (IsInit<2) //initialization stage
			{
				if (selected==-1)SelectPlayer(x,y); //player selection
				else if (ppointers[selected]->moved==0) //moving player to new field (checking if it is available)
				{
					if((x<0)|(x>10)|(y<1)|(y>19))MessageBox(hWnd,"You can not move palyers outside of the field!","Warning",0);
					else if(((selected<4)&(y<10))|((selected>3)&(y>10)))MessageBox(hWnd,"You can not place palyers on the opposite team's side!","Warning",0); 
					else if(((team_to_move==0)&(selected>3))|((team_to_move==1)&(selected<4)))MessageBox(hWnd,"You can not move palyers of the opposite team!","Warning",0); 
					else
					{
						int occupied = 0;
						for(int k=0; k<8; k++)
						{
							if ((k!=selected)&(ppointers[k]->xNew==x)&(ppointers[k]->yNew==y))
							{
								occupied=1; 
								break;
							}
						}
						if(occupied)MessageBox(hWnd,"This field is occupied!","Warning",0); 
						else
						{
							ppointers[selected]->xNew=x;
							ppointers[selected]->yNew=y;
							ppointers[selected]->moved=1;
						}
					} 
					selected=-1;
				}
				else selected =-1;
			}
			else //game stage
			{	
				if (selected==-1)
				{
					SelectPlayer(x,y);
					if (selected!=-1)
					{
						if(ppointers[selected]->moved == 0)CountDestinations(hdc);
					}
				}
				else if(ppointers[selected]->moved == 0)
				{
					if((x<0)|(x>10)|(y<1)|(y>19))MessageBox(hWnd,"You can not move palyers outside of the field!","Warning",0);
					else if(((team_to_move==0)&(selected>3))|((team_to_move==1)&(selected<4)))MessageBox(hWnd,"You can not move palyers of the opposite team!","Warning",0); 
					else 
					{
						if((ppointers[selected]->x!=x)|(ppointers[selected]->y!=y))
						{
							for (i=0;i<ppointers[selected]->fields_available; i++)
							{
								if((ppointers[selected]->dest[i][0]==x)&(ppointers[selected]->dest[i][1]==y))
								{
									ppointers[selected]->xNew=x;
									ppointers[selected]->yNew=y;
									if(ball_posession==selected)
									{
										Ball.x=ppointers[selected]->xNew;
										Ball.y=ppointers[selected]->yNew;
									}
									else if((ball_posession==-10)&(Ball.x==x)&(Ball.y==y))
									{
										ball_posession=selected;
										Ball.acquired = 1;
										Touch(selected/4, FALSE);
									}
									ppointers[selected]->moved=1;
									break;
								}
							}
						}
						if(ppointers[selected]->moved!=1)MessageBox(hWnd,"No move","Warning",0);
					} 
				
					selected=-1;
				}
				else selected = -1;
			}
		}
		DrawField(hdc);		
		break;
		
    case WM_DESTROY:
		ReleaseDC(hWnd,hdc);
		PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return 0;
}

BOOL DrawField(HDC hdc) //main graphics engine
{
	int i;
	HBRUSH hBrushS,hBrushH;
	CHAR Scoreboard [20];
	RECT Pass, Shot, Attack, TakeBack, EndTurn;
	//
	Pass.left=500;
	Pass.right=600;
	Pass.top=200;
	Pass.bottom=250;

	Shot.left=500;
	Shot.right=600;
	Shot.top=300;
	Shot.bottom=350;

	Attack.left=500;
	Attack.right=600;
	Attack.top=400;
	Attack.bottom=450;

	TakeBack.left=500;
	TakeBack.right=600;
	TakeBack.top=500;
	TakeBack.bottom=550;

	EndTurn.left=500;
	EndTurn.right=600;
	EndTurn.top=600;
	EndTurn.bottom=650;
	
	
	//drawing score
	sprintf_s(Scoreboard, "White - Black %d : %d", White.score, Black.score);
	TextOut(hdc, 500, 50, Scoreboard, (int)strlen(Scoreboard));
	TextOut(hdc, 500, 100, TeamName[team_to_move], (int)strlen(TeamName[team_to_move]));
	sprintf_s(Scoreboard,"Time %d min", (int)match_time);
	TextOut(hdc, 600, 100, Scoreboard, (int)strlen(Scoreboard));


	//drawing buttons
	if ((selected==ball_posession)&(IsInit>1)&(ball_posession/4==team_to_move)&(passed==0))hBrushS=CreateSolidBrush(RGB(0,255,0));
	else hBrushS=CreateSolidBrush(RGB(255,255,255));
	SelectObject(hdc,hBrushS);
	Rectangle(hdc,Pass.left,Pass.top,Pass.right,Pass.bottom);
	DeleteObject(hBrushS);
	DrawText(hdc,"Pass",4,&Pass,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
	
	if ((selected==ball_posession)&(IsInit>1)&(passed==0)&(ball_posession/4==team_to_move))hBrushS=CreateSolidBrush(RGB(0,0,255));
	else hBrushS=CreateSolidBrush(RGB(255,255,255));
	SelectObject(hdc,hBrushS);
	Rectangle(hdc,Shot.left,Shot.top,Shot.right,Shot.bottom);
	DrawText(hdc,"Shot",4,&Shot,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
	DeleteObject(hBrushS);
	hBrushS=CreateSolidBrush(AttackKind(selected) ? RGB(255,0,0) : RGB(255,255,255)); // same rule as the Attack button
	SelectObject(hdc,hBrushS);
	Rectangle(hdc,Attack.left,Attack.top,Attack.right,Attack.bottom);
	DrawText(hdc,"Attack",6,&Attack,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
	DeleteObject(hBrushS);
	if (take_back ==0)hBrushS=CreateSolidBrush(RGB(0,255,255));
	else hBrushS=CreateSolidBrush(RGB(255,255,255));
	SelectObject(hdc,hBrushS);
	Rectangle(hdc,TakeBack.left,TakeBack.top,TakeBack.right,TakeBack.bottom);
	DrawText(hdc,"Take Back",9,&TakeBack,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
	DeleteObject(hBrushS);
	
	hBrushS=CreateSolidBrush(RGB(150,150,150)); 
	SelectObject(hdc,hBrushS);
	Rectangle(hdc,EndTurn.left,EndTurn.top,EndTurn.right,EndTurn.bottom);
	DrawText(hdc,"End Turn",8,&EndTurn,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
	DeleteObject(hBrushS);
	//drawing the field
	hBrushS=CreateSolidBrush(RGB(0,255,0)); 
	SelectObject(hdc,hBrushS);
	for (int i=1;i<20; i++)
	{
		for (int j = 0; j<11; j++)
		{
		  Rectangle(hdc,j*CELL_SIZE,i*CELL_SIZE,(j+1)*CELL_SIZE,(i+1)*CELL_SIZE);
		}
	
	}
	DeleteObject(hBrushS);
	//drawing the goals
	hBrushS=CreateSolidBrush(RGB(255,255,0));
	SelectObject(hdc,hBrushS);
	Rectangle(hdc,4*CELL_SIZE,0,7*CELL_SIZE,CELL_SIZE);
	Rectangle(hdc,4*CELL_SIZE,20*CELL_SIZE,7*CELL_SIZE,21*CELL_SIZE);
	DeleteObject(hBrushS);

	//drawing fields available for pass
	if (passed==1)
	{
		int center_x = Ball.x+(ppointers[selected]->xNew>ppointers[selected]->xPrev)-(ppointers[selected]->xNew<ppointers[selected]->xPrev);
		int center_y = Ball.y+(ppointers[selected]->yNew>ppointers[selected]->yPrev)-(ppointers[selected]->yNew<ppointers[selected]->yPrev);
		int leftlimit =(center_x-4>0)?center_x-4:0;
		int bottomlimit =(center_y-4>1)?center_y-4:1;
		int rightlimit =(center_x+4>10)?10:center_x+4;
		int toplimit =(center_y+4>19)?19:center_y+4;
		hBrushS=CreateSolidBrush(RGB(200,200,0));
		SelectObject(hdc,hBrushS);
		for (int i=bottomlimit;i<=toplimit; i++)
		{	
			for (int j = leftlimit; j<=rightlimit; j++)
			{
				Rectangle(hdc,j*CELL_SIZE,i*CELL_SIZE,(j+1)*CELL_SIZE,(i+1)*CELL_SIZE);
			}
	
		}
		DeleteObject(hBrushS);
	}

	// drawing team white
	hBrushS=CreateSolidBrush(RGB(255,255,255));
	hBrushH=CreateSolidBrush(RGB(200,200,200));
	
	for (i=0;i<4; i++)
	{
		//drawing player
		if (ppointers[i]->moved==0)
		{
			SelectObject(hdc,hBrushS);
			Rectangle(hdc, ppointers[i]->xNew*CELL_SIZE,ppointers[i]->yNew*CELL_SIZE,(ppointers[i]->xNew+1)*CELL_SIZE,(ppointers[i]->yNew+1)*CELL_SIZE);
		}
		else
		{
			SelectObject(hdc,hBrushH);
			Rectangle(hdc, ppointers[i]->xNew*CELL_SIZE,ppointers[i]->yNew*CELL_SIZE,(ppointers[i]->xNew+1)*CELL_SIZE,(ppointers[i]->yNew+1)*CELL_SIZE);
		}
		
		//vectors of movement
		MoveToEx(hdc,ppointers[i]->xNew*CELL_SIZE+CELL_SIZE/2,ppointers[i]->yNew*CELL_SIZE+CELL_SIZE/2,NULL);
		if(IsInit==2)
		{
			if(ppointers[i]->moved==1)
			{
				LineTo(hdc,(2*ppointers[i]->xNew-ppointers[i]->x)*CELL_SIZE+CELL_SIZE/2,(2*ppointers[i]->yNew-ppointers[i]->y)*CELL_SIZE+CELL_SIZE/2);
			}
			else
			{
				LineTo(hdc,(2*ppointers[i]->xNew-ppointers[i]->xPrev)*CELL_SIZE+CELL_SIZE/2,(2*ppointers[i]->yNew-ppointers[i]->yPrev)*CELL_SIZE+CELL_SIZE/2);
			}
		}
	}
	DeleteObject(hBrushS);
	DeleteObject(hBrushH);
	//darwing team black
	hBrushS=CreateSolidBrush(RGB(0,0,0));
	hBrushH=CreateSolidBrush(RGB(75,75,75));
	for (i=4;i<8; i++)
	{
		
		//drawing player
		if (ppointers[i]->moved==0)
		{
			SelectObject(hdc,hBrushS);
			Rectangle(hdc, ppointers[i]->xNew*CELL_SIZE,ppointers[i]->yNew*CELL_SIZE,(ppointers[i]->xNew+1)*CELL_SIZE,(ppointers[i]->yNew+1)*CELL_SIZE);
		}
		else
		{
			SelectObject(hdc,hBrushH);
			Rectangle(hdc, ppointers[i]->xNew*CELL_SIZE,ppointers[i]->yNew*CELL_SIZE,(ppointers[i]->xNew+1)*CELL_SIZE,(ppointers[i]->yNew+1)*CELL_SIZE);
		}

		
		//Vectors of movement
		MoveToEx(hdc,ppointers[i]->xNew*CELL_SIZE+CELL_SIZE/2,ppointers[i]->yNew*CELL_SIZE+CELL_SIZE/2,NULL);
		if(IsInit==2)
		{
			if(ppointers[i]->moved==1)
			{
				LineTo(hdc,(2*ppointers[i]->xNew-ppointers[i]->x)*CELL_SIZE+CELL_SIZE/2,(2*ppointers[i]->yNew-ppointers[i]->y)*CELL_SIZE+CELL_SIZE/2);
			}
			else
			{
				LineTo(hdc,(2*ppointers[i]->xNew-ppointers[i]->xPrev)*CELL_SIZE+CELL_SIZE/2,(2*ppointers[i]->yNew-ppointers[i]->yPrev)*CELL_SIZE+CELL_SIZE/2);
			}
		}
	}

	DeleteObject(hBrushS);
	DeleteObject(hBrushH);

	
	// darwing selected player
	if (selected!=-1)
	{
		if(IsInit==2)
		{
			hBrushS=CreateSolidBrush(RGB(0,255,255));
			SelectObject(hdc,hBrushS);

		//darwing available fields
			if(ppointers[selected]->moved==0)
			{
				for (int k=0; k<ppointers[selected]->fields_available; k++)
				{
					Rectangle(hdc, 
					ppointers[selected]->dest[k][0]*CELL_SIZE,
					ppointers[selected]->dest[k][1]*CELL_SIZE,
					(ppointers[selected]->dest[k][0]+1)*CELL_SIZE,
					(ppointers[selected]->dest[k][1]+1)*CELL_SIZE);
				}
			}
			DeleteObject(hBrushS);
		}

		hBrushS=CreateSolidBrush(RGB(0,0,255));
		SelectObject(hdc,hBrushS);

		Rectangle(hdc, 
			ppointers[selected]->xNew*CELL_SIZE,
			ppointers[selected]->yNew*CELL_SIZE,
			(ppointers[selected]->xNew+1)*CELL_SIZE,
			(ppointers[selected]->yNew+1)*CELL_SIZE);
		DeleteObject(hBrushS);
		
	}
	//drawing the ball
	hBrushS=CreateSolidBrush(RGB(255,0,0));
	SelectObject(hdc,hBrushS);
	Ellipse(hdc, Ball.x*CELL_SIZE+5,Ball.y*CELL_SIZE+5,(Ball.x+1)*CELL_SIZE-5,(Ball.y+1)*CELL_SIZE-5);
	DeleteObject(hBrushS);
	if((ball_posession==-10)&&((Ball.vx!=0)||(Ball.vy!=0))) //drawing ball trajectory
	{
		int temp_x=Ball.x, temp_y=Ball.y, temp_vx=Ball.vx, temp_vy=Ball.vy;
		int temp_color = 200;
		BOOL stopX, stopY;
		while ((temp_vx!=0)||(temp_vy!=0))
		{
			temp_x+=temp_vx;
			temp_y+=temp_vy;
			StopAtLines(temp_x, temp_y, temp_vx, temp_vy, stopX, stopY);
			if (stopX) temp_vx = 0;
			if (stopY) temp_vy = 0;
			hBrushS=CreateSolidBrush(RGB(temp_color,temp_color,0));
			SelectObject(hdc,hBrushS);
			Ellipse(hdc, temp_x*CELL_SIZE+5,temp_y*CELL_SIZE+5,(temp_x+1)*CELL_SIZE-5,(temp_y+1)*CELL_SIZE-5);
			DeleteObject(hBrushS);
			if(temp_vx) temp_vx+=Ball.decx;
			if(temp_vy) temp_vy+=Ball.decy;
			if(temp_color>40) temp_color-=40;
		}
	}

	return TRUE;
}
BOOL ResetField() //resets field to default
{
	// white keeper 
	White.Keeper.x=5;
	White.Keeper.y=19;
	White.Keeper.vx=0;
	White.Keeper.vy=0;

	//white players
	White.Players[0].xNew=0;
	White.Players[0].yNew=11;
	White.Players[0].x=0;
	White.Players[0].y=11;
	White.Players[0].xPrev=0;
	White.Players[0].yPrev=11;
	White.Players[0].moved=0;
	White.Players[1].xNew=0;
	White.Players[1].yNew=12;
	White.Players[1].x=0;
	White.Players[1].y=12;
	White.Players[1].xPrev=0;
	White.Players[1].yPrev=12;
	White.Players[1].moved=0;
	White.Players[2].xNew=0;
	White.Players[2].yNew=13;
	White.Players[2].x=0;
	White.Players[2].y=13;
	White.Players[2].xPrev=0;
	White.Players[2].yPrev=13;
	White.Players[2].moved=0;
	White.Players[3].xNew=0;
	White.Players[3].yNew=14;
	White.Players[3].x=0;
	White.Players[3].y=14;
	White.Players[3].xPrev=0;
	White.Players[3].yPrev=14;
	White.Players[3].moved=0;

	//black keeper
	Black.Keeper.x=5;
	Black.Keeper.y=0;
	Black.Keeper.vx=0;
	Black.Keeper.vy=0;

	Black.Players[0].xNew=10;
	Black.Players[0].yNew=9;
	Black.Players[0].x=10;
	Black.Players[0].y=9;
	Black.Players[0].xPrev=10;
	Black.Players[0].yPrev=9;
	Black.Players[0].moved=0;
	Black.Players[1].xNew=10;
	Black.Players[1].yNew=8;
	Black.Players[1].x=10;
	Black.Players[1].y=8;
	Black.Players[1].xPrev=10;
	Black.Players[1].yPrev=8;
	Black.Players[1].moved=0;
	Black.Players[2].xNew=10;
	Black.Players[2].yNew=7;
	Black.Players[2].x=10;
	Black.Players[2].y=7;
	Black.Players[2].xPrev=10;
	Black.Players[2].yPrev=7;
	Black.Players[2].moved=0;
	Black.Players[3].xNew=10;
	Black.Players[3].yNew=6;
	Black.Players[3].x=10;
	Black.Players[3].y=6;
	Black.Players[3].xPrev=10;
	Black.Players[3].yPrev=6;
	Black.Players[3].moved=0;
	Ball.x=5;
	Ball.xPrev=5;
	Ball.y=10;
	Ball.yPrev=10;
	Ball.vx=0;
	Ball.vy=0;
	IsInit=0;
	for (int k=0; k<8; k++) ppointers[k]->attacked=0;
	keeper_restart=-1;
	keeper_backpass=FALSE;
	last_pass=FALSE;
	seal_hist[0]=0;
	seal_hist[1]=0;
	selected =-1;
	ball_posession=-10;
	passed = 0;
	take_back = 0;
					
	return true;
}
BOOL SelectPlayer(int x, int y) //player selection by mouse click
{
	int i;

	selected = -1;
	for (i=0; i<8;i++)
	{
		if((ppointers[i]->xNew==x)&&(ppointers[i]->yNew==y)) 
		{
			selected = i;
			break;
		}
	}
	return true;
}
BOOL BallPosession() //checks which player posesses the ball
{
	int i;
	ball_posession=-10;
	for (i=0; i<8;i++)
	{
		if((ppointers[i]->xNew==Ball.x)&&(ppointers[i]->yNew==Ball.y)) 
		{
			ball_posession = i;
			Ball.vx = 0; // the ball is stopped by the player
			Ball.vy = 0;
			Touch(i/4, FALSE);
			break;
		}
	}
	return true;
}
BOOL AttackPlayer(HWND hWnd) //tackle: the selected player tries to take the ball from the ball holder
{
	int deflections[24][2];
	int count=0;
	player* holder = ppointers[ball_posession];
	player* attacker = ppointers[selected];
	attacker->attacked=1;
	attacker->atk_dx = holder->xNew-attacker->xNew; // a later move in this turn must not go against it
	attacker->atk_dy = holder->yNew-attacker->yNew;
	float result = (float)rand()/RAND_MAX;
	float inc =(team_to_move)?0.05f:-0.05f;
	float probability = 0.35f -0.05f*(holder->x!=attacker->xNew)+inc*(holder->y-attacker->yNew);

	if(result<probability) //attacking player got the ball
	{
		MessageBox(hWnd,"attacking player got the ball;","Status",0);
		ball_posession = selected;
		Ball.x = attacker->xNew;
		Ball.y = attacker->yNew;
		Ball.vx = 0;
		Ball.vy = 0;
		Touch(selected/4, FALSE);
		return true;
	}
	if(result<probability+0.2f) //ball lost control: random cell in the 5x5 square around the holder
	{
		for (int i = holder->x-2; i <= holder->x+2; i++)
			for (int j = holder->y-2; j <= holder->y+2; j++)
				if(InField(i,j) && !((i==holder->x)&&(j==holder->y)) && !((i==attacker->xNew)&&(j==attacker->yNew)))
				{
					deflections[count][0] = i;
					deflections[count][1] = j;
					count++;
				}
		if(count>0)
		{
			MessageBox(hWnd,"ball lost control","Status",0);
			int n = RandIndex(count);
			Ball.x = deflections[n][0];
			Ball.y = deflections[n][1];
			Ball.vx=0;
			Ball.vy=0;
			Touch(selected/4, FALSE);
			BallPosession();
			if (ball_posession==-10) KeeperCheck(Ball.x, Ball.y);
			return true;
		}
	}
	//player under attack keeps the ball
	MessageBox(hWnd,"player under attack keeps the ball","Status",0);
	Ball.x = holder->x;
	Ball.y = holder->y;
	return true;
}

BOOL PokeBall(HWND hWnd) //fight for a free ball sealed by two opponents: diagonal attack through a closed corridor
{
	int deflections[24][2];
	int count=0, n=0;
	int team = selected/4;
	player* attacker = ppointers[selected];
	attacker->attacked=1;
	attacker->atk_dx = Ball.x-attacker->xNew; // a later move in this turn must not go against it
	attacker->atk_dy = Ball.y-attacker->yNew;
	for (int k=0; k<8; k++) // opponents next to the ball
		if ((k/4!=team)&&(abs(ppointers[k]->xNew-Ball.x)<=1)&&(abs(ppointers[k]->yNew-Ball.y)<=1)) n++;
	float probability = (n<=2) ? 0.5f : ((n==3) ? 0.35f : 0.25f);
	float result = (float)rand()/RAND_MAX;
	if(result<probability) //the ball is poked out to the attacker
	{
		MessageBox(hWnd,"The ball is poked out!","Status",0);
		ball_posession = selected;
		Ball.x = attacker->xNew;
		Ball.y = attacker->yNew;
		Ball.vx = 0;
		Ball.vy = 0;
		Touch(team, FALSE);
		return true;
	}
	if(result<probability+0.2f) //ball knocked away: random cell in the 5x5 square around the ball
	{
		for (int i = Ball.x-2; i <= Ball.x+2; i++)
			for (int j = Ball.y-2; j <= Ball.y+2; j++)
				if(InField(i,j) && !((i==Ball.x)&&(j==Ball.y)) && !((i==attacker->xNew)&&(j==attacker->yNew)))
				{
					deflections[count][0] = i;
					deflections[count][1] = j;
					count++;
				}
		MessageBox(hWnd,"The ball is knocked away","Status",0);
		int r = RandIndex(count);
		Ball.x = deflections[r][0];
		Ball.y = deflections[r][1];
		Ball.vx = 0;
		Ball.vy = 0;
		Touch(team, FALSE);
		BallPosession();
		if (ball_posession==-10) KeeperCheck(Ball.x, Ball.y);
		return true;
	}
	MessageBox(hWnd,"The ball stays where it is","Status",0);
	return true;
}

int GoalShot(HWND hWnd) //calculates result of shot
{
	int deflections[15][2];
	int status;
	int count=0;
	int k, leftlimit, rightlimit;
	float result, probability, distance, block_distance;
	player* shooter = ppointers[selected];
	int target = (team_to_move)?20:0;
	result = (float)rand()/RAND_MAX;
	distance = (float)((shooter->xNew-5)*(shooter->xNew-5)+(shooter->yNew-target)*(shooter->yNew-target));
	probability = expf(-0.033f*distance); //dependence on distance
	probability = probability * (1 - 0.15f*abs(shooter->xNew-5)); //dependence on angle
	// shot line: from the centre of the shooter's cell to the centre of the goal
	float sx = shooter->xNew+0.5f, sy = shooter->yNew+0.5f;
	float lx = 5.5f-sx, ly = (target+0.5f)-sy;
	float len = sqrtf(lx*lx+ly*ly); // > 0: the shooter is always inside the field
	// blocking players reduce probability
	leftlimit = (shooter->xNew<4)?shooter->xNew:4;
	rightlimit =(shooter->xNew>6)?shooter->xNew:6;
	for (k=0; k<8; k++)
	{
		player* p = ppointers[k];
		//check if the player is in shot blocking area
		if((k!=selected)&&(p->xNew>=leftlimit)&&(p->xNew<=rightlimit)&&
		   (((team_to_move==0)&&(p->yNew<=shooter->yNew))||((team_to_move==1)&&(p->yNew>=shooter->yNew))))
		{
			block_distance = fabsf(lx*((p->yNew+0.5f)-sy)-ly*((p->xNew+0.5f)-sx))/len;
			probability = probability*(1 -0.4f/(1+block_distance));
		}
	}

	if(result<probability)//goal
	{
		MessageBox(hWnd,"Goal!!!","Status",0);
		status =1;
	}
	else if(result<1-probability+probability*probability)//keeper caught the ball
	{
		MessageBox(hWnd,"keeper caught the ball","Status",0);
		status =-1;
	}
	else //keeper reflected the ball: random cell x=3..7 in the 3 rows in front of the goal
	{
		MessageBox(hWnd,"keeper reflected the ball","Status",0);
		status =0;
		int lLimit = (team_to_move)?16:2; // rows 2..4 (or 16..18): never into the goal mouth
		for (int i = 3; i < 8; i++)
			for (int j = lLimit; j < lLimit+3; j++)
			{
				deflections[count][0] = i;
				deflections[count][1] = j;
				count++;
			}
		int n = RandIndex(count);
		ball_posession = -10;
		Ball.x = deflections[n][0];
		Ball.y = deflections[n][1];
		Ball.vx = 0;
		Ball.vy = 0;
		Touch(team_to_move^1, FALSE); // touched by the keeper
	}
	BallPosession();
	return status;
}

BOOL CountDestinations(HDC hdc) //Calculates possible destinations for selected player
{
	int i,j,k,x,y, block1 =0,block2 =0,block3 =0,block4 =0, count =0, occupied=0;
	float a,b, cross;

	x = ppointers[selected]->x+(ppointers[selected]->x > ppointers[selected]->xPrev)-(ppointers[selected]->x < ppointers[selected]->xPrev);
	y = ppointers[selected]->y+(ppointers[selected]->y > ppointers[selected]->yPrev)-(ppointers[selected]->y < ppointers[selected]->yPrev);
	
	for (i=-1;i<2; i++)
	{
		for(j=-1;j<2;j++)
		{
			//checking if the field is occupied by any other player
			occupied =0;
			block1 = 0;
			block2 = 0;
			block3 = 0;
			block4 = 0;
			for (k=0; k<8; k++)

			{
				if((k!=selected)&(ppointers[k]->xNew>x-2)&(ppointers[k]->xNew<x+2)&(ppointers[k]->yNew>y-2)&(ppointers[k]->yNew<y+2))
				//check if the player is within available area
				{
					//same field
					occupied = occupied|((ppointers[k]->xNew==x+i)&&(ppointers[k]->yNew==y+j));

					// vertical blocking
					occupied = occupied|(((ppointers[k]->yNew<y+j)&(ppointers[selected]->y<ppointers[k]->yNew))&
									(ppointers[k]->xNew==ppointers[selected]->x)&((x+i)==ppointers[selected]->x));
					occupied = occupied|(((ppointers[k]->yNew>y+j)&(ppointers[selected]->y>ppointers[k]->yNew))&
									(ppointers[k]->xNew==ppointers[selected]->x)&((x+i)==ppointers[selected]->x));
					 
					if ((x+i)!=ppointers[selected]->x)
					{
						a = (float(((y+j)-ppointers[selected]->y))/((x+i)-ppointers[selected]->x));
						b = float((x+i+0.5)*(ppointers[selected]->y+0.5)-(ppointers[selected]->x+0.5)*(y+j+0.5))/((x+i)-ppointers[selected]->x);
						cross = float(abs(a*(ppointers[k]->xNew+0.5)-(ppointers[k]->yNew+0.5)+b)/sqrt(a*a+1));
						occupied = occupied|((cross<0.707)&((x+i-ppointers[k]->xNew)*(ppointers[k]->xNew-ppointers[selected]->x)>=0)*((y+j-ppointers[k]->yNew)*(ppointers[k]->yNew-ppointers[selected]->y)>=0));
						if((cross>0.706)&(cross<0.708)&(occupied==0)) //checking "box" conditions
						{
							block1=block1|(((x+i)==ppointers[k]->xNew)&(ppointers[k]->yNew==(y+j+ppointers[selected]->y)/2));
							block2=block2|(((y+j)==ppointers[k]->yNew)&(ppointers[k]->xNew==(x+i+ppointers[selected]->x)/2));
							int inc = ((y+j)>ppointers[selected]->y)?1:-1;
							block3=block3|((ppointers[k]->xNew==ppointers[selected]->x)&(ppointers[k]->yNew==(ppointers[selected]->y+inc)));
							inc = ((x+i)>ppointers[selected]->x)?1:-1;
							block4=block4|((ppointers[selected]->y==ppointers[k]->yNew)&(ppointers[k]->xNew==(ppointers[selected]->x+inc)));

						}
					}
				}
			}
			occupied = occupied|(block1&block2)|(block3&block4);
			// a player who has already attacked in this turn can not move against the direction of his attack
			if (ppointers[selected]->attacked &&
				((x+i-ppointers[selected]->x)*ppointers[selected]->atk_dx+(y+j-ppointers[selected]->y)*ppointers[selected]->atk_dy < 0))
				occupied = 1;
			if(((x+i)>=0)&((x+i)<11)&((y+j)>=1)&((y+j)<20)&(occupied==0)) //checking if player is within the field
			{
				
				ppointers[selected]->dest[count][0]=x+i;
				ppointers[selected]->dest[count][1]=y+j;
				count++;
			}
		}
	}
	ppointers[selected]->fields_available=count;
	ppointers[selected]->initialized=1;
	return true;
}

BOOL Intercept(HWND hWnd, int exclude) //Checks if the ball is intercepted on its way (Ball.x,Ball.y) -> (Ball.x+vx,Ball.y+vy)
{
	int deflections[24][2];
	int order[8];
	float along[8];
	int n = 0, k;
	int ex = Ball.x+Ball.vx, ey = Ball.y+Ball.vy;
	BOOL stopX, stopY;
	StopAtLines(ex, ey, Ball.vx, Ball.vy, stopX, stopY);
	int dx = ex-Ball.x, dy = ey-Ball.y; // actual displacement of this step
	float v2 = (float)(dx*dx+dy*dy);

	// candidates: players inside the bounding box of the segment, except the excluded one (the passer)
	// and the one standing in the end cell (he simply receives the ball);
	// they are checked in the order the ball reaches them
	if (v2 > 0)
	{
		for(k=0; k<8; k++)
		{
			int px = ppointers[k]->xNew, py = ppointers[k]->yNew;
			if (k==exclude) continue;
			if ((px==ex)&&(py==ey)) continue;
			if (((Ball.x-px)*(px-ex) < 0) || ((Ball.y-py)*(py-ey) < 0)) continue;
			float t = ((px-Ball.x)*dx + (py-Ball.y)*dy)/v2;
			int m = n++;
			while (m>0 && along[m-1] > t) { along[m]=along[m-1]; order[m]=order[m-1]; m--; }
			along[m]=t;
			order[m]=k;
		}
	}

	for (int c=0; c<n; c++)
	{
		k = order[c];
		player* p = ppointers[k];
		// distance from the player's cell centre to the ball line
		float dist = fabsf(dx*(float)(p->yNew-Ball.y) - dy*(float)(p->xNew-Ball.x))/sqrtf(v2);
		float probability = 0.6f*expf(-2.0f*dist*dist);
		float result = (float)rand()/RAND_MAX;
		if(result<probability*probability) //ball interception
		{
			MessageBox(hWnd,"Ball intercepted!","Status",0);
			Ball.x=p->xNew;
			Ball.y=p->yNew;
			Ball.vx=0;
			Ball.vy=0;
			Touch(k/4, FALSE);
			ball_posession=k;
			selected =-1;
			return true;
		}
		if(result<probability) //ball deflection: random cell of the field in the 5x5 square around the player
		{
			int count=0;
			for (int i = p->xNew-2; i <= p->xNew+2; i++)
				for (int j = p->yNew-2; j <= p->yNew+2; j++)
					if(InField(i,j) && ((i!=p->xNew)||(j!=p->yNew)))
					{
						deflections[count][0] = i;
						deflections[count][1] = j;
						count++;
					}
			int r = RandIndex(count);
			Ball.x = deflections[r][0];
			Ball.y = deflections[r][1];
			Ball.vx=0;
			Ball.vy=0;
			Touch(k/4, FALSE);
			BallPosession();
			if (ball_posession==-10) KeeperCheck(Ball.x, Ball.y);
			MessageBox(hWnd,"Ball interception - deflected!","Status",0);
			selected =-1;
			return true;
		}
	}
	// nobody touched the ball: it reaches the end of the segment
	int mx, my;
	BOOL mouth = FindMouthCell(Ball.x, Ball.y, ex, ey, mx, my);
	Ball.x=ex;
	Ball.y=ey;
	if (stopX) Ball.vx = 0; // the ball reached a side line
	if (stopY) Ball.vy = 0; // the ball reached an end line
	ball_posession=-10;
	if (mouth) // the ball came into a free goal mouth cell: the keeper takes it there
	{
		Ball.x = mx;
		Ball.y = my;
		KeeperCheck(mx, my);
	}
	selected =-1;
	return true;
}
