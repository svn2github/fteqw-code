#include "quakedef.h"

#ifdef _WIN32
//not really needed, but nice none-the-less.
#include "winquake.h"
LONG CDAudio_MessageHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	return 1;
}
#endif

void CDAudio_Shutdown(void)
{
}

void CDAudio_Update(void)
{
}

void CDAudio_Init(void)
{
}

void CDAudio_Play(int track)
{
}

void CDAudio_Stop(void)
{
}

void CDAudio_Pause(void)
{
}

void CDAudio_Resume(void)
{
}

void CDAudio_Eject(void)
{
}

void CDAudio_CloseDoor(void)
{
}

int CDAudio_GetAudioDiskInfo(void)
{
	return -2;
}

int CDAudio_Startup(void)
{
	return -2;
}