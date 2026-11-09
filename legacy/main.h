BOOL DrawField(HDC hdc);
//BOOL DrawPlayer(HDC hdc);
BOOL ResetField(); 
BOOL SelectPlayer(int x,int y);
BOOL BallPosession();
BOOL CountDestinations(HDC hdc);
BOOL AttackPlayer(HWND hWnd);
BOOL PokeBall(HWND hWnd);
int GoalShot(HWND hWnd);
BOOL Intercept(HWND hWnd, int exclude);

struct player
{
	int x;
	int y;
	int xNew;
	int yNew;
	int xPrev;
	int yPrev;
	int fields_available;
	int dest[9][2];
	int yellowcards;
	int moved;
	int initialized;
	int attacked;
	int atk_dx; // direction of the attack made in this turn (to the ball)
	int atk_dy;
};
struct goalkeeper
{
	int x;
	int y;
	int vx;
	int vy;
	int yellowcards;
	BOOL ballpossession;
};
struct team
{
	player Players[4];
	goalkeeper Keeper;
	int score;
};
struct ball
{
	int x;
	int y;
	int xPrev;
	int yPrev;
	int vx;
	int vy;
	int decx;
	int decy;
	int update;
	int posession;
	int acquired;
	int last_touch;
};