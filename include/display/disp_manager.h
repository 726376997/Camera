#ifndef DISP_MANAGER_H_
#define DISP_MANAGER_H_

#define FB_DEVICES_NAME "/dev/fb0" /* fb的设备名 */

typedef struct DispBuff
{
    int iXres;
    int iYres;
    int iBpp;
    unsigned char *buff;
} DispBuff, *PDispBuff;

typedef struct DispOpr
{
    char *name;
    int (*DispInit)(void);
    void (*DispExit)(void);
    void (*GetDispBuff)(PDispBuff ptDispBuff);
    void (*CleanScreen)(unsigned int Color);
    struct DispOpr *ptNext;
} DispOpr, *PDispOpr;

void RegisterDispOpr(PDispOpr ptDispOpr);
void DispSystemRegister(void);
int SelectDefaultDispDevAndInit(char *name);
void DispDevExit(void);
void GetDispBuffer(PDispBuff ptDispBuff);
void CleanScreen(unsigned int Color);

void FrameBufferRegister(void);

#endif /* DISP_MANAGER_H_ */
