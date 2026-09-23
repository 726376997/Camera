#include "display/disp_manager.h"
#include "common/logger.h"

#include <string.h>

static PDispOpr g_ptDispOprHead = NULL;
static PDispOpr g_ptDefaultDispOpr = NULL;

void RegisterDispOpr(PDispOpr ptDispOpr)
{
    ptDispOpr->ptNext = g_ptDispOprHead;
    g_ptDispOprHead = ptDispOpr;
}

void DispSystemRegister(void)
{
    FrameBufferRegister();
}

int SelectDefaultDispDevAndInit(char *name)
{
    PDispOpr ptTmp;
    int ret;
    for (ptTmp = g_ptDispOprHead; ptTmp; ptTmp = ptTmp->ptNext)
    {
        if (strcmp(ptTmp->name, name) == 0)
        {
            g_ptDefaultDispOpr = ptTmp;
            ret = g_ptDefaultDispOpr->DispInit();
            if (ret < 0)
            {
                ERR_PRINTF("SelectDefaultDispOprAndInit: DispInit failed\n");
                return -1;
            }
            return 0;
        }
    }
    return -1;
}

void DispDevExit(void)
{
    g_ptDefaultDispOpr->DispExit();
}

void GetDispBuffer(PDispBuff ptDispBuff)
{
    g_ptDefaultDispOpr->GetDispBuff(ptDispBuff);
}

void CleanScreen(unsigned int Color)
{
    g_ptDefaultDispOpr->CleanScreen(Color);
}
