#include "camera/camera_manager.h"
#include "common/logger.h"
#include <string.h>

static CameraDevice *g_ptCamDevHead = NULL;
static CameraDevice *g_ptDefaultCamOpr = NULL;

void RegisterCamDev(CameraDevice *ptCamDev)
{
    ptCamDev->ptNext = g_ptCamDevHead;
    g_ptCamDevHead = ptCamDev;
}

void CameraSystemRegister(void)
{
    USBCameraRegister();
}

int SelectDefaultCamDevAndInit(char *name)
{
    CameraDevice *ptTmp;

    for (ptTmp = g_ptCamDevHead; ptTmp; ptTmp = ptTmp->ptNext)
    {
        if (strcmp(ptTmp->cam_attr.name, name) == 0)
        {
            g_ptDefaultCamOpr = ptTmp;
            if (g_ptDefaultCamOpr->CamDevInit() < 0)
            {
                ERR_PRINTF("SelectDefaultCamOprAndInit: CamInit failed\n");
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

void ShowFormat(void)
{
    g_ptDefaultCamOpr->ShowFormat();
}

int SetFormat(int width, int height, int pixelformat)
{
    if (g_ptDefaultCamOpr->SetFormat(width, height, pixelformat) < 0)
    {
        ERR_PRINTF("SetFormat failed\n");
        return -1;
    }

    return 0;
}

int ReqBuf(void)
{
    if (g_ptDefaultCamOpr->ReqBuf() < 0)
    {
        ERR_PRINTF("ReqBuf failed\n");
        return -1;
    }

    return 0;
}

int StreamOn(void)
{
    if (g_ptDefaultCamOpr->StreamOn() < 0)
    {
        ERR_PRINTF("StreamOn failed\n");
        return -1;
    }

    return 0;
}

int StreamOff(void)
{
    if (g_ptDefaultCamOpr->StreamOff() < 0)
    {
        ERR_PRINTF("StreamOff failed\n");
        return -1;
    }

    return 0;
}

int GetCameraFrame(CameraFrame *buf, int timeout_ms)
{
    if (g_ptDefaultCamOpr->GetCameraFrame(buf, timeout_ms) < 0)
    {
        // ERR_PRINTF("GetCameraFrame failed\n");
        return -1;
    }

    return 0;
}

int PutCameraFrame(const CameraFrame *buf)
{
    if (g_ptDefaultCamOpr->PutCameraFrame(buf) < 0)
    {
        ERR_PRINTF("PutCameraFrame failed\n");
        return -1;
    }

    return 0;
}

int CamDevExit(void)
{
    if (g_ptDefaultCamOpr->CamDevExit() < 0)
    {
        ERR_PRINTF("CamDevExit failed\n");
        return -1;
    }

    return 0;
}