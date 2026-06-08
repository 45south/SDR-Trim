/*
 * sdrtrimgui.c  —  SDR Trim GUI  —  Win32 front-end for sdrtrim.exe
 *
 * Build (MSYS2/MinGW64):
 *   gcc -Wall -O2 -o sdrtrimgui.exe sdrtrimgui.c sdrtrimgui.res \
 *       -lcomctl32 -lcomdlg32 -lshell32 -mwindows -municode
 *
 * Place sdrtrimgui.exe in the same folder as sdrtrim.exe.
 * Settings saved to sdrtrimgui.ini in the same folder.
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Control IDs                                                        */
/* ------------------------------------------------------------------ */
#define ID_INPUT_EDIT       101
#define ID_BROWSE           102
#define ID_MODE_FULL        103
#define ID_MODE_TRIM        104
#define ID_START_EDIT       105
#define ID_END_EDIT         106
#define ID_CHAN_DUAL        107
#define ID_CHAN_CH1         108
#define ID_CHAN_CH2         109
#define ID_FMT_SAME         110
#define ID_FMT_LINRAD       111
#define ID_FMT_WAVVIEWDX    112
#define ID_FMT_SDRUNO       113
#define ID_FMT_SDRCONNECT   114
#define ID_DDC_CHECK        115
#define ID_DDC_CENTRE       116
#define ID_DDC_BW           117
#define ID_LOG_EDIT         118
#define ID_RUN              119
#define ID_CLEAR            120
#define ID_OUTFILE_STATIC   121
#define ID_PROGRESS         122
#define ID_HEADER_PANEL     123
#define ID_CANCEL           124
#define ID_PCT_STATIC       125
#define ID_ETA_STATIC       126
/* Batch */
#define ID_ADD_BATCH        130
#define ID_RUN_BATCH        131
#define ID_CLEAR_BATCH      132
#define ID_BATCH_LIST       133
#define ID_BATCH_LABEL      134
#define ID_LOG_GROUP        135

/* Custom messages */
#define WM_APPENDLOG    (WM_USER + 1)  /* wp=0 lp=heap wchar_t*; wp=1 done, lp=exitcode */
#define WM_UPDATEPROG   (WM_USER + 2)  /* wp=percent */
#define WM_UPDATEETA    (WM_USER + 3)  /* lp=heap wchar_t* */
#define WM_BATCHSTATUS  (WM_USER + 4)  /* wp=job_index, lp=heap wchar_t* status */
#define WM_BATCHDONE    (WM_USER + 5)  /* batch run finished */

/* ListView columns */
#define COL_NUM     0
#define COL_INPUT   1
#define COL_OP      2
#define COL_OUTPUT  3
#define COL_STATUS  4

/* ------------------------------------------------------------------ */
/*  Colours                                                            */
/* ------------------------------------------------------------------ */
#define CLR_HEADER_BG   RGB(45,  45,  48)
#define CLR_HEADER_TEXT RGB(240, 240, 240)
#define CLR_RUN_BG      RGB(0,   120, 215)
#define CLR_RUN_HOT     RGB(0,   102, 180)
#define CLR_RUN_TEXT    RGB(255, 255, 255)
#define CLR_BG          RGB(245, 245, 245)
#define CLR_SEG_ON      RGB(0,   180,   0)
#define CLR_SEG_OFF     RGB(220, 220, 220)

/* ------------------------------------------------------------------ */
/*  Batch job structure                                                */
/* ------------------------------------------------------------------ */
#define MAX_JOBS 64
typedef struct {
    wchar_t cmdline[4096];
    wchar_t op_desc[128];
    wchar_t input_short[64];
    wchar_t output_short[64];
    wchar_t output_full[MAX_PATH]; /* full path for overwrite check */
    wchar_t status[32];
} BatchJob;

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */
static HWND      g_hwnd;
static HINSTANCE g_hinst;
static BOOL      g_running    = FALSE;
static BOOL      g_batch_run  = FALSE;
static HANDLE    g_thread     = NULL;
static HANDLE    g_hprocess   = NULL;
static int       g_progress   = 0;
static int       g_header_h   = 52;
static HBRUSH    g_hbr_bg     = NULL;
static HBRUSH    g_hbr_header = NULL;
static HFONT     g_font       = NULL;
static HFONT     g_font_bold  = NULL;
static HFONT     g_font_mono  = NULL;
static HFONT     g_font_title = NULL;
static wchar_t   g_exedir[MAX_PATH];

static BatchJob  g_jobs[MAX_JOBS];
static int       g_job_count  = 0;

/* ------------------------------------------------------------------ */
/*  INI helpers                                                        */
/* ------------------------------------------------------------------ */
static void ini_path(wchar_t *buf,int n)
{ _snwprintf(buf,n,L"%s\\sdrtrimgui.ini",g_exedir); }

static void ini_ws(const wchar_t *sec,const wchar_t *key,const wchar_t *val)
{ wchar_t p[MAX_PATH]; ini_path(p,MAX_PATH); WritePrivateProfileStringW(sec,key,val,p); }

static void ini_wi(const wchar_t *sec,const wchar_t *key,int v)
{ wchar_t b[32]; _snwprintf(b,32,L"%d",v); ini_ws(sec,key,b); }

static void ini_rs(const wchar_t *sec,const wchar_t *key,const wchar_t *def,wchar_t *buf,int n)
{ wchar_t p[MAX_PATH]; ini_path(p,MAX_PATH); GetPrivateProfileStringW(sec,key,def,buf,n,p); }

static int ini_ri(const wchar_t *sec,const wchar_t *key,int def)
{ wchar_t p[MAX_PATH]; ini_path(p,MAX_PATH); return (int)GetPrivateProfileIntW(sec,key,def,p); }

/* ------------------------------------------------------------------ */
/*  Save / restore settings                                            */
/* ------------------------------------------------------------------ */
static void update_channel_controls(void);  /* forward declaration */
static void update_format_controls(void);    /* forward declaration */
static void update_run_batch_button(void);   /* forward declaration */

static void save_settings(void)
{
    wchar_t b[MAX_PATH];
    ini_wi(L"S",L"Trim",  IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)?1:0);
    GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),b,8); ini_ws(L"S",L"Start",b);
    GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  b,8); ini_ws(L"S",L"End",  b);
    ini_wi(L"S",L"Chan",
        IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1)?1:
        IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2)?2:0);
    ini_wi(L"S",L"Fmt",
        IsDlgButtonChecked(g_hwnd,ID_FMT_LINRAD)    ?1:
        IsDlgButtonChecked(g_hwnd,ID_FMT_WAVVIEWDX) ?2:
        IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO)    ?3:
        IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT)?4:0);
    ini_wi(L"S",L"DDC", IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)?1:0);
    GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),b,16); ini_ws(L"S",L"DDCcf",b);
    GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_BW),    b,16); ini_ws(L"S",L"DDCbw",b);
}

static void restore_settings(void)
{
    wchar_t b[MAX_PATH];
    int trim=ini_ri(L"S",L"Trim",0);
    CheckRadioButton(g_hwnd,ID_MODE_FULL,ID_MODE_TRIM,trim?ID_MODE_TRIM:ID_MODE_FULL);
    EnableWindow(GetDlgItem(g_hwnd,ID_START_EDIT),trim);
    EnableWindow(GetDlgItem(g_hwnd,ID_END_EDIT),  trim);
    ini_rs(L"S",L"Start",L"",b,8); SetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),b);
    ini_rs(L"S",L"End",  L"",b,8); SetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  b);
    int chan=ini_ri(L"S",L"Chan",0);
    CheckRadioButton(g_hwnd,ID_CHAN_DUAL,ID_CHAN_CH2,
        chan==1?ID_CHAN_CH1:chan==2?ID_CHAN_CH2:ID_CHAN_DUAL);
    int fmt=ini_ri(L"S",L"Fmt",0);
    CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_SDRCONNECT,
        fmt==1?ID_FMT_LINRAD:fmt==2?ID_FMT_WAVVIEWDX:
        fmt==3?ID_FMT_SDRUNO:fmt==4?ID_FMT_SDRCONNECT:ID_FMT_SAME);
    int ddc=ini_ri(L"S",L"DDC",0);
    CheckDlgButton(g_hwnd,ID_DDC_CHECK,ddc?BST_CHECKED:BST_UNCHECKED);
    EnableWindow(GetDlgItem(g_hwnd,ID_DDC_CENTRE),ddc);
    EnableWindow(GetDlgItem(g_hwnd,ID_DDC_BW),    ddc);
    ini_rs(L"S",L"DDCcf",L"",b,16); SetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),b);
    ini_rs(L"S",L"DDCbw",L"",b,16); SetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_BW),    b);
    update_format_controls();
}

/* ------------------------------------------------------------------ */
/*  Log helpers                                                        */
/* ------------------------------------------------------------------ */
static void log_append(const wchar_t *t)
{
    HWND h=GetDlgItem(g_hwnd,ID_LOG_EDIT);
    int n=GetWindowTextLengthW(h);
    SendMessageW(h,EM_SETSEL,n,n);
    SendMessageW(h,EM_REPLACESEL,FALSE,(LPARAM)t);
    int lines=(int)SendMessageW(h,EM_GETLINECOUNT,0,0);
    SendMessageW(h,EM_LINESCROLL,0,lines);
}
static void log_clear(void){ SetWindowTextW(GetDlgItem(g_hwnd,ID_LOG_EDIT),L""); }

/* ------------------------------------------------------------------ */
/*  Output filename prediction — file probing                         */
/* ------------------------------------------------------------------ */
typedef struct {
    int  fmt;
    int  dual;
    int  sample_rate;
    int  centre_hz;
    int  year,mon,day,hour,min,sec;
    int  utc_digit;
} FileInfo;

static uint32_t rd32(const uint8_t *b){ return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
static uint16_t rd16(const uint8_t *b){ return (uint16_t)b[0]|((uint16_t)b[1]<<8); }

static int64_t utc_to_unix(int y,int mo,int d,int h,int mi,int s)
{
    static const int md[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    int y0=y-1,y1=1969;
    int leaps=(y0/4-y1/4)-(y0/100-y1/100)+(y0/400-y1/400);
    int64_t days=(int64_t)(y-1970)*365+leaps;
    int leap=((y%4==0&&y%100!=0)||y%400==0);
    for(int m=1;m<mo;m++) days+=md[m]+(m==2?leap:0);
    days+=d-1;
    return days*86400LL+(int64_t)h*3600+(int64_t)mi*60+s;
}

static void unix_to_utc(int64_t ep,int*y,int*mo,int*d,int*h,int*mi,int*s)
{
    int64_t r=ep;
    *s=(int)(r%60); r/=60; *mi=(int)(r%60); r/=60; *h=(int)(r%24); r/=24;
    int yr=1970;
    while(1){ int dy=((yr%4==0&&yr%100!=0)||yr%400==0)?366:365; if(r<dy)break; r-=dy; yr++; }
    *y=yr;
    static const int md[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    int lp=((yr%4==0&&yr%100!=0)||yr%400==0);
    int m=1; while(m<=12){ int dim=md[m]+(m==2?lp:0); if(r<dim)break; r-=dim; m++; }
    *mo=m; *d=(int)r+1;
}

static BOOL probe_file(const wchar_t *path,FileInfo *fi)
{
    memset(fi,0,sizeof(*fi)); fi->fmt=-1;
    FILE *f=_wfopen(path,L"rb");
    if(!f) return FALSE;
    /* Read enough to cover large JUNK chunks (SDR Connect uses 28-byte JUNK,
       but read generously in case of variation) */
    uint8_t hdr[1024]; size_t got=fread(hdr,1,sizeof(hdr),f);
    fclose(f);
    if(got<12) return FALSE;

    /* ── Linrad raw: magic FF FF FF FF ── */
    if(hdr[0]==0xFF&&hdr[1]==0xFF&&hdr[2]==0xFF&&hdr[3]==0xFF){        fi->fmt=0;
        uint64_t u64=0; for(int i=0;i<8;i++) u64|=(uint64_t)hdr[12+i]<<(i*8);
        double pc; memcpy(&pc,&u64,8); fi->centre_hz=(int)(pc*1e6+0.5);
        fi->dual=(rd32(hdr+32)==4)?1:0;
        fi->sample_rate=(int)rd32(hdr+36);
        u64=0; for(int i=0;i<8;i++) u64|=(uint64_t)hdr[4+i]<<(i*8);
        double ts; memcpy(&ts,&u64,8); int64_t ep=(int64_t)ts;
        unix_to_utc(ep,&fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec);
        const wchar_t *bn=wcsrchr(path,L'\\'); if(!bn)bn=path; else bn++;
        if(wcslen(bn)>16){ wchar_t uc=bn[15]; fi->utc_digit=(uc>=L'0'&&uc<=L'9')?(int)(uc-L'0'):0; }
        return TRUE;
    }

    /* ── RIFF/WAVE and RF64 formats ── */
    int is_rf64 = (hdr[0]=='R'&&hdr[1]=='F'&&hdr[2]=='6'&&hdr[3]=='4'&&
                   hdr[8]=='W'&&hdr[9]=='A'&&hdr[10]=='V'&&hdr[11]=='E');
    int is_riff = (hdr[0]=='R'&&hdr[1]=='I'&&hdr[2]=='F'&&hdr[3]=='F'&&
                   hdr[8]=='W'&&hdr[9]=='A'&&hdr[10]=='V'&&hdr[11]=='E');
    if(is_riff||is_rf64){

        /* Walk chunks from offset 12 */
        uint32_t pos=12;
        int have_fmt=0;
        BOOL is_sdrconnect=0;

        while(pos+8<=(uint32_t)got){
            char id[5]={0}; memcpy(id,hdr+pos,4);
            uint32_t csz=rd32(hdr+pos+4);

            if(memcmp(id,"JUNK",4)==0){
                /* SDR Connect marker */
                is_sdrconnect=1;
                pos+=8+csz;
                continue;
            }
            if(memcmp(id,"fmt ",4)==0){
                /* PCM fmt: channels at +2, sample_rate at +4 (all relative to chunk data start) */
                if(pos+8+10<=(uint32_t)got){
                    fi->sample_rate=(int)rd32(hdr+pos+12);  /* pos+8+4 */
                    uint16_t nch=rd16(hdr+pos+10);           /* pos+8+2 */
                    fi->dual=(nch==4)?1:0;
                }
                have_fmt=1;
                pos+=8+csz; if(csz&1)pos++;
                continue;
            }
            if(memcmp(id,"auxi",4)==0 && have_fmt){
                /* SDRuno auxi chunk: SYSTEMTIME start (16 bytes) at chunk data */
                /* SYSTEMTIME: wYear(2) wMonth(2) wDayOfWeek(2) wDay(2)
                               wHour(2) wMinute(2) wSecond(2) wMilliseconds(2) */
                uint32_t ad=pos+8; /* auxi data start */
                if(ad+40<=(uint32_t)got){
                    fi->year =(int)rd16(hdr+ad+0);
                    fi->mon  =(int)rd16(hdr+ad+2);
                    /* ad+4 = wDayOfWeek, skip */
                    fi->day  =(int)rd16(hdr+ad+6);
                    fi->hour =(int)rd16(hdr+ad+8);
                    fi->min  =(int)rd16(hdr+ad+10);
                    fi->sec  =(int)rd16(hdr+ad+12);
                    /* centre_freq at ad+32 (after start SYSTEMTIME(16) + stop SYSTEMTIME(16)) */
                    fi->centre_hz=(int)(int32_t)rd32(hdr+ad+32);
                }
                fi->fmt=2; /* SDRuno */
                /* If auxi gave zero date, fall back to filename */
                if(fi->year==0){
                    const wchar_t *bn=wcsrchr(path,L'\\'); if(!bn)bn=path; else bn++;
                    wchar_t stem[MAX_PATH]; wcsncpy(stem,bn,MAX_PATH-1);
                    wchar_t *dot=wcsrchr(stem,L'.'); if(dot)*dot=L'\0';
                    int cf_khz=0;
                    swscanf(stem,L"SDRuno_%4d%2d%2d_%2d%2d%2dZ_%dkHz",
                        &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&cf_khz) ||
                    swscanf(stem,L"SDRuno_%4d%2d%2d_%2d%2d%2d_%dkHz",
                        &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&cf_khz);
                    if(cf_khz>0&&fi->centre_hz==0) fi->centre_hz=cf_khz*1000;
                }
                return TRUE;
            }
            if(memcmp(id,"data",4)==0) break;
            pos+=8+csz; if(csz&1)pos++;
        }

        /* If we found a fmt chunk but no auxi: SDR Connect */
        if(have_fmt && is_sdrconnect){
            fi->fmt=3;
            /* Metadata from filename */
            const wchar_t *bn=wcsrchr(path,L'\\'); if(!bn)bn=path; else bn++;
            wchar_t stem[MAX_PATH]; wcsncpy(stem,bn,MAX_PATH-1);
            wchar_t *dot=wcsrchr(stem,L'.'); if(dot)*dot=L'\0';
            int ff=0;
            swscanf(stem,L"SDRconnect_IQ_%4d%2d%2d_%2d%2d%2d_%dHZ",
                &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&ff);
            fi->centre_hz=ff;
            return TRUE;
        }
        /* SDRuno with no auxi found (data chunk reached first) */
        if(have_fmt && !is_sdrconnect){
            fi->fmt=2;
            /* Try to get timestamp and frequency from filename as fallback */
            const wchar_t *bn=wcsrchr(path,L'\\'); if(!bn)bn=path; else bn++;
            wchar_t stem[MAX_PATH]; wcsncpy(stem,bn,MAX_PATH-1);
            wchar_t *dot=wcsrchr(stem,L'.'); if(dot)*dot=L'\0';
            int cf_khz=0;
            swscanf(stem,L"SDRuno_%4d%2d%2d_%2d%2d%2dZ_%dkHz",
                &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&cf_khz) ||
            swscanf(stem,L"SDRuno_%4d%2d%2d_%2d%2d%2d_%dkHz",
                &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&cf_khz);
            if(cf_khz>0&&fi->centre_hz==0) fi->centre_hz=cf_khz*1000;
            return TRUE;
        }
        return FALSE;
    }

    /* ── WavViewDX raw: headerless, identified by filename ── */
    {
        const wchar_t *bn=wcsrchr(path,L'\\'); if(!bn)bn=path; else bn++;
        wchar_t stem[MAX_PATH]; wcsncpy(stem,bn,MAX_PATH-1);
        wchar_t *dot=wcsrchr(stem,L'.'); if(dot)*dot=L'\0';
        wchar_t *pat=wcsstr(stem,L"iq_pcm16_ch");
        if(pat){
            fi->fmt=1; int ch=0; swscanf(pat+11,L"%d",&ch); fi->dual=(ch==2)?1:0;
            wchar_t *cf=wcsstr(pat,L"_cf"); if(cf) swscanf(cf+3,L"%d",&fi->centre_hz);
            wchar_t *sr=wcsstr(pat,L"_sr"); if(sr) swscanf(sr+3,L"%d",&fi->sample_rate);
            wchar_t *dt=wcsstr(pat,L"_dt");
            if(dt) swscanf(dt+3,L"%4d%2d%2d-%2d%2d%2d",
                &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL predict_outfile(const wchar_t *inpath,wchar_t *outfile,int n)
{
    FileInfo fi;
    if(!probe_file(inpath,&fi)) return FALSE;
    int out_fmt;
    if     (IsDlgButtonChecked(g_hwnd,ID_FMT_LINRAD))     out_fmt=0;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_WAVVIEWDX))  out_fmt=1;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO))     out_fmt=2;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT)) out_fmt=3;
    else out_fmt=fi.fmt;
    if(out_fmt<0) out_fmt=0;
    int dual_out=fi.dual;
    if(IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1)||IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2)) dual_out=0;
    int cf=fi.centre_hz;
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t buf[16]; GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),buf,16);
        int ddc_cf=_wtoi(buf); if(ddc_cf>0) cf=ddc_cf*1000;
    }
    int sy=fi.year,smo=fi.mon,sd=fi.day,sh=fi.hour,smi=fi.min;
    if(IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)){
        wchar_t buf[8]; int hh=0,mm=0;
        GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),buf,8);
        if(wcslen(buf)==4) swscanf(buf,L"%2d%2d",&hh,&mm);
        int64_t file_ep=utc_to_unix(fi.year,fi.mon,fi.day,fi.hour,fi.min,fi.sec);
        int64_t start_ep=utc_to_unix(fi.year,fi.mon,fi.day,hh,mm,0);
        if(start_ep<file_ep){
            int64_t next=start_ep+86400; int dummy_s;
            unix_to_utc(next,&sy,&smo,&sd,&sh,&smi,&dummy_s);
            smi=mm; sh=hh;
        } else { sy=fi.year; smo=fi.mon; sd=fi.day; sh=hh; smi=mm; }
    }
    int sr=fi.sample_rate;
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t buf[16]; GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_BW),buf,16);
        int bw_khz=_wtoi(buf);
        if(bw_khz>0&&sr>0){ int D=sr/(bw_khz*1000); if(D<1)D=1; sr=sr/D; }
    }
    wchar_t name[MAX_PATH];
    switch(out_fmt){
    case 0: _snwprintf(name,MAX_PATH,L"%04d%02d%02d_%02d%02d%02d%d_%dkHz.raw",
                sy,smo,sd,sh,smi,0,fi.utc_digit,cf/1000); break;
    case 1: _snwprintf(name,MAX_PATH,L"iq_pcm16_ch%d_cf%d_sr%d_dt%04d%02d%02d-%02d%02d%02d.raw",
                dual_out?2:1,cf,sr,sy,smo,sd,sh,smi,0); break;
    case 2: _snwprintf(name,MAX_PATH,L"SDRuno_%04d%02d%02d_%02d%02d%02dZ_%dkHz.wav",
                sy,smo,sd,sh,smi,0,cf/1000); break;
    case 3: _snwprintf(name,MAX_PATH,L"SDRconnect_IQ_%04d%02d%02d_%02d%02d%02d_%dHZ.wav",
                sy,smo,sd,sh,smi,0,cf); break;
    default: return FALSE;
    }
    wchar_t dir[MAX_PATH]; wcsncpy(dir,inpath,MAX_PATH-1);
    wchar_t *sl=wcsrchr(dir,L'\\'); if(sl){ sl[1]=L'\0'; } else { dir[0]=L'\0'; }
    _snwprintf(outfile,n,L"%s%s",dir,name);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Build command line from current GUI state                         */
/* ------------------------------------------------------------------ */
static BOOL build_cmdline(wchar_t *cmd,int n)
{
    wchar_t input[MAX_PATH];
    GetWindowTextW(GetDlgItem(g_hwnd,ID_INPUT_EDIT),input,MAX_PATH);
    if(!input[0]){
        MessageBoxW(g_hwnd,L"Please select an input file.",L"SDR Trim",MB_OK|MB_ICONWARNING);
        return FALSE;
    }
    wchar_t exe[MAX_PATH];
    _snwprintf(exe,MAX_PATH,L"%s\\sdrtrim.exe",g_exedir);
    _snwprintf(cmd,n,L"\"%s\" \"%s\"",exe,input);
    if(IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)){
        wchar_t s[8],e[8];
        GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),s,8);
        GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  e,8);
        if(wcslen(s)!=4||wcslen(e)!=4){
            MessageBoxW(g_hwnd,L"Start and end times must each be 4 digits (e.g. 0300).",
                L"SDR Trim",MB_OK|MB_ICONWARNING);
            return FALSE;
        }
        wchar_t t[MAX_PATH]; _snwprintf(t,MAX_PATH,L"%s %s %s",cmd,s,e);
        wcsncpy(cmd,t,n-1);
    }
    if     (IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1)) wcsncat(cmd,L" --ch1",n-wcslen(cmd)-1);
    else if(IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2)) wcsncat(cmd,L" --ch2",n-wcslen(cmd)-1);
    if     (IsDlgButtonChecked(g_hwnd,ID_FMT_LINRAD))     wcsncat(cmd,L" linrad",    n-wcslen(cmd)-1);
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_WAVVIEWDX))  wcsncat(cmd,L" wavviewdx", n-wcslen(cmd)-1);
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO))     wcsncat(cmd,L" sdruno",    n-wcslen(cmd)-1);
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT)) wcsncat(cmd,L" sdrconnect",n-wcslen(cmd)-1);
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t cf[16],bw[16];
        GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),cf,16);
        GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_BW),    bw,16);
        if(!cf[0]||!bw[0]){
            MessageBoxW(g_hwnd,L"DDC is enabled but centre or bandwidth is empty.",
                L"SDR Trim",MB_OK|MB_ICONWARNING);
            return FALSE;
        }
        wchar_t d[64]; _snwprintf(d,64,L" --ddc %s %s",cf,bw);
        wcsncat(cmd,d,n-wcslen(cmd)-1);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Build operation description for batch list                        */
/* ------------------------------------------------------------------ */
static void build_op_desc(wchar_t *desc,int n)
{
    wchar_t parts[128]={0};
    /* Mode */
    if(IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)){
        wchar_t s[8]={0},e[8]={0};
        GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),s,8);
        GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  e,8);
        _snwprintf(parts,64,L"Trim %s-%s",s,e);
    } else {
        wcsncpy(parts,L"Full file",64);
    }
    /* Channel */
    if     (IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1)) wcsncat(parts,L" ch1",128-wcslen(parts)-1);
    else if(IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2)) wcsncat(parts,L" ch2",128-wcslen(parts)-1);
    /* Format */
    const wchar_t *fmt=L"";
    if     (IsDlgButtonChecked(g_hwnd,ID_FMT_LINRAD))     fmt=L" \u2192 linrad";
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_WAVVIEWDX))  fmt=L" \u2192 wavviewdx";
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO))     fmt=L" \u2192 sdruno";
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT)) fmt=L" \u2192 sdrconnect";
    wcsncat(parts,fmt,128-wcslen(parts)-1);
    /* DDC */
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t cf[16]={0},bw[16]={0};
        GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),cf,16);
        GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_BW),    bw,16);
        wchar_t ddc[48]; _snwprintf(ddc,48,L" DDC %skHz/%skHz",cf,bw);
        wcsncat(parts,ddc,128-wcslen(parts)-1);
    }
    wcsncpy(desc,parts,n-1);
}

/* ------------------------------------------------------------------ */
/*  Update prediction label                                           */
/* ------------------------------------------------------------------ */
static void update_channel_controls(void)
{
    wchar_t input[MAX_PATH];
    GetWindowTextW(GetDlgItem(g_hwnd,ID_INPUT_EDIT),input,MAX_PATH);
    FileInfo fi; fi.dual=1; /* default: assume dual if unknown */
    if(input[0]) probe_file(input,&fi);
    BOOL dual = fi.dual;
    EnableWindow(GetDlgItem(g_hwnd,ID_CHAN_DUAL), dual);
    EnableWindow(GetDlgItem(g_hwnd,ID_CHAN_CH2),  dual);
    /* If single-channel, force Tuner A and uncheck Dual/Tuner B */
    if(!dual){
        CheckRadioButton(g_hwnd,ID_CHAN_DUAL,ID_CHAN_CH2,ID_CHAN_CH1);
    }
}

static void update_run_batch_button(void)
{
    if(g_running){ EnableWindow(GetDlgItem(g_hwnd,ID_RUN_BATCH),FALSE); return; }
    BOOL any_runnable=FALSE;
    for(int i=0;i<g_job_count;i++){
        if(wcscmp(g_jobs[i].status,L"Done")!=0)
            { any_runnable=TRUE; break; }
    }
    EnableWindow(GetDlgItem(g_hwnd,ID_RUN_BATCH), any_runnable && g_job_count>0);
}


/* Estimate output data bytes for current settings.
   Returns 0 if unknown. Used to check RF64 threshold. */
static uint64_t estimate_output_bytes(const wchar_t *input, const FileInfo *fi)
{
    if(!input[0] || fi->fmt<0 || fi->sample_rate<=0) return 0;

    /* Get input file size */
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if(!GetFileAttributesExW(input,GetFileExInfoStandard,&fa)) return 0;
    uint64_t fsz=((uint64_t)fa.nFileSizeHigh<<32)|fa.nFileSizeLow;

    /* Bytes per frame in input */
    int in_ch = fi->dual ? 4 : 2;
    uint64_t in_data = fsz; /* approximate — close enough for threshold check */

    /* Output channels */
    int out_ch = (IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1)||
                  IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2)||!fi->dual) ? 2 : 4;

    /* Scale by channel ratio */
    uint64_t out_data = (in_data / (uint64_t)in_ch) * (uint64_t)out_ch;

    /* Scale by trim ratio if trimming */
    if(IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)){
        wchar_t sb[8]={0},eb[8]={0}; int sh=0,sm=0,eh=0,em=0;
        GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),sb,8);
        GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  eb,8);
        if(wcslen(sb)==4&&wcslen(eb)==4){
            swscanf(sb,L"%2d%2d",&sh,&sm);
            swscanf(eb,L"%2d%2d",&eh,&em);
            int start_min=sh*60+sm, end_min=eh*60+em;
            if(end_min<=start_min) end_min+=24*60; /* midnight crossing */
            int trim_min=end_min-start_min;
            /* Total recording duration in minutes */
            uint64_t rec_bytes = in_data;
            int bytes_per_sec = fi->sample_rate * in_ch * 2;
            int total_sec = (bytes_per_sec>0)?(int)(rec_bytes/bytes_per_sec):0;
            if(total_sec>0){
                int total_min = total_sec/60; if(total_min<1) total_min=1;
                if(trim_min<total_min)
                    out_data = (out_data * (uint64_t)trim_min) / (uint64_t)total_min;
            }
        }
    }

    /* DDC decimation */
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t bw[16]={0}; GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_BW),bw,16);
        int bw_khz=_wtoi(bw);
        if(bw_khz>0&&fi->sample_rate>0){
            int D=fi->sample_rate/(bw_khz*1000); if(D<1)D=1;
            out_data/=(uint64_t)D;
        }
    }
    return out_data;
}
static void update_format_controls(void)
{
    /* Grey out format buttons that would produce a same-format no-op.
     * A format is a no-op if: output fmt == input fmt AND no trim AND
     * no DDC AND no channel extraction (ch1/ch2 from dual). */
    wchar_t input[MAX_PATH];
    GetWindowTextW(GetDlgItem(g_hwnd,ID_INPUT_EDIT),input,MAX_PATH);

    /* Default: all enabled */
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SAME),      TRUE);
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_LINRAD),     TRUE);
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_WAVVIEWDX),  TRUE);
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SDRUNO),     TRUE);
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SDRCONNECT), TRUE);

    if(!input[0]) return;

    FileInfo fi; fi.fmt=-1; fi.dual=0;
    probe_file(input,&fi);
    if(fi.fmt<0) return;

    BOOL trimming   = IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM);
    BOOL ddc        = IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK);
    /* ch1/ch2 only counts as extraction when input is dual-channel */
    BOOL ch_extract = fi.dual &&
                      (IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1) ||
                       IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2));
    BOOL dual_out   = fi.dual && !ch_extract;

    /* For dual output, sdruno and sdrconnect are never valid (single-tuner only) */
    if(dual_out){
        /* Disable WAV formats — they can't hold dual-tuner data */
        BOOL wav_sel = IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO) ||
                       IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT);
        if(wav_sel)
            CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_SDRCONNECT,
                fi.fmt==0?ID_FMT_LINRAD:ID_FMT_WAVVIEWDX);
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SDRUNO),     FALSE);
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SDRCONNECT), FALSE);
        /* "Same as input" for dual linrad/wavviewdx is only a no-op if no trim/ddc */
        if(!trimming && !ddc){
            int noop_id = (fi.fmt==0)?ID_FMT_LINRAD:
                          (fi.fmt==1)?ID_FMT_WAVVIEWDX:-1;
            if(noop_id>=0){
                if(IsDlgButtonChecked(g_hwnd,ID_FMT_SAME)||
                   IsDlgButtonChecked(g_hwnd,noop_id)){
                    int other = (noop_id==ID_FMT_LINRAD)?ID_FMT_WAVVIEWDX:ID_FMT_LINRAD;
                    CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_SDRCONNECT,other);
                }
                EnableWindow(GetDlgItem(g_hwnd,noop_id),    FALSE);
                EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SAME),FALSE);
            }
        }
        return;
    }

    /* Single output: disable same-format if it would be a no-op */
    if(trimming || ddc) return;

    int noop_id = -1;
    switch(fi.fmt){
        case 0: noop_id = ID_FMT_LINRAD;     break;
        case 1: noop_id = ID_FMT_WAVVIEWDX;  break;
        case 2: noop_id = ID_FMT_SDRUNO;     break;
        case 3: noop_id = ID_FMT_SDRCONNECT; break;
    }
    if(noop_id<0) return;

    if(IsDlgButtonChecked(g_hwnd,ID_FMT_SAME)||IsDlgButtonChecked(g_hwnd,noop_id)){
        int fallbacks[]={ID_FMT_LINRAD,ID_FMT_WAVVIEWDX,ID_FMT_SDRUNO,ID_FMT_SDRCONNECT};
        for(int i=0;i<4;i++){
            if(fallbacks[i]!=noop_id){
                CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_SDRCONNECT,fallbacks[i]);
                break;
            }
        }
    }
    EnableWindow(GetDlgItem(g_hwnd,noop_id),    FALSE);
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SAME),FALSE);
}

static void update_prediction(void)
{
    wchar_t input[MAX_PATH];
    GetWindowTextW(GetDlgItem(g_hwnd,ID_INPUT_EDIT),input,MAX_PATH);
    if(!input[0]){
        SetWindowTextW(GetDlgItem(g_hwnd,ID_OUTFILE_STATIC),L"Output: (select input file)");
        return;
    }
    wchar_t outfile[MAX_PATH];
    if(predict_outfile(input,outfile,MAX_PATH)){
        wchar_t label[MAX_PATH+16];
        wchar_t *fn=wcsrchr(outfile,L'\\');
        _snwprintf(label,MAX_PATH+16,L"Output: %s",fn?fn+1:outfile);
        SetWindowTextW(GetDlgItem(g_hwnd,ID_OUTFILE_STATIC),label);
    } else {
        SetWindowTextW(GetDlgItem(g_hwnd,ID_OUTFILE_STATIC),L"Output: (unable to predict)");
    }
}

/* ------------------------------------------------------------------ */
/*  Batch ListView helpers                                            */
/* ------------------------------------------------------------------ */
static void batch_list_refresh_row(int idx)
{
    HWND hlv=GetDlgItem(g_hwnd,ID_BATCH_LIST);
    wchar_t num[8]; _snwprintf(num,8,L"%d",idx+1);
    LVITEMW li; ZeroMemory(&li,sizeof(li));
    li.mask=LVIF_TEXT; li.iItem=idx;
    li.iSubItem=COL_NUM;    li.pszText=num;                        ListView_SetItem(hlv,&li);
    li.iSubItem=COL_INPUT;  li.pszText=g_jobs[idx].input_short;   ListView_SetItem(hlv,&li);
    li.iSubItem=COL_OP;     li.pszText=g_jobs[idx].op_desc;       ListView_SetItem(hlv,&li);
    li.iSubItem=COL_OUTPUT; li.pszText=g_jobs[idx].output_short;  ListView_SetItem(hlv,&li);
    li.iSubItem=COL_STATUS; li.pszText=g_jobs[idx].status;        ListView_SetItem(hlv,&li);
}

static void batch_list_add_row(int idx)
{
    HWND hlv=GetDlgItem(g_hwnd,ID_BATCH_LIST);
    LVITEMW li; ZeroMemory(&li,sizeof(li));
    li.mask=LVIF_TEXT; li.iItem=idx; li.iSubItem=0;
    wchar_t num[8]; _snwprintf(num,8,L"%d",idx+1);
    li.pszText=num;
    ListView_InsertItem(hlv,&li);
    batch_list_refresh_row(idx);
}

static void batch_list_set_status(int idx,const wchar_t *status)
{
    wcsncpy(g_jobs[idx].status,status,31);
    HWND hlv=GetDlgItem(g_hwnd,ID_BATCH_LIST);
    LVITEMW li; ZeroMemory(&li,sizeof(li));
    li.mask=LVIF_TEXT; li.iItem=idx; li.iSubItem=COL_STATUS;
    li.pszText=(wchar_t*)status;
    ListView_SetItem(hlv,&li);
}

/* ------------------------------------------------------------------ */
/*  Core job runner — runs one sdrtrim process, streams output        */
/*  Called from both single-run thread and batch thread.              */
/* ------------------------------------------------------------------ */
static DWORD run_one_job(const wchar_t *cmdline)
{
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE pr=NULL,pw=NULL;
    if(!CreatePipe(&pr,&pw,&sa,0)){
        PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(L"Error: pipe failed.\r\n"));
        return (DWORD)-1;
    }
    SetHandleInformation(pr,HANDLE_FLAG_INHERIT,0);

    STARTUPINFOW si; ZeroMemory(&si,sizeof(si));
    si.cb=sizeof(si);
    si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE;
    si.hStdOutput=pw; si.hStdError=pw;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);

    /* Need mutable copy for CreateProcessW */
    wchar_t mutable_cmd[4096];
    wcsncpy(mutable_cmd,cmdline,4095);

    PROCESS_INFORMATION pi; ZeroMemory(&pi,sizeof(pi));
    if(!CreateProcessW(NULL,mutable_cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi)){
        wchar_t msg[256];
        _snwprintf(msg,256,L"Error: could not launch sdrtrim.exe (error %lu).\r\n"
            L"Ensure sdrtrim.exe is in the same folder as sdrtrimgui.exe.\r\n",GetLastError());
        PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(msg));
        CloseHandle(pr); CloseHandle(pw);
        return (DWORD)-1;
    }
    CloseHandle(pw);
    g_hprocess=pi.hProcess;

    char raw[512]; DWORD nread;
    wchar_t linebuf[1024]; int linelen=0;
    while(ReadFile(pr,raw,sizeof(raw)-1,&nread,NULL)&&nread>0){
        for(DWORD i=0;i<nread;i++){
            char c=raw[i];
            if(c=='\r'){
                if(linelen>0){
                    linebuf[linelen]=L'\0';
                    wchar_t *pct=wcsstr(linebuf,L"%");
                    if(pct&&pct>linebuf){
                        wchar_t *p=pct-1;
                        while(p>linebuf&&*(p-1)>=L'0'&&*(p-1)<=L'9') p--;
                        if(*p>=L'0'&&*p<=L'9'){
                            int val=_wtoi(p);
                            if(val>=0&&val<=100)
                                PostMessageW(g_hwnd,WM_UPDATEPROG,(WPARAM)val,0);
                        }
                        wchar_t *eta=wcsstr(pct,L"ETA ");
                        if(!eta) eta=wcsstr(pct,L"Done ");
                        if(eta){
                            wchar_t etabuf[32]; int ei=0;
                            wchar_t *ep=eta;
                            while(*ep&&*ep!=L'\r'&&*ep!=L'\n'&&ei<31) etabuf[ei++]=*ep++;
                            etabuf[ei]=L'\0';
                            PostMessageW(g_hwnd,WM_UPDATEETA,0,(LPARAM)_wcsdup(etabuf));
                        }
                    }
                }
                linelen=0;
            } else if(c=='\n'){
                linebuf[linelen++]=L'\r'; linebuf[linelen++]=L'\n'; linebuf[linelen]=L'\0';
                PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(linebuf));
                linelen=0;
            } else {
                if(linelen<1020){
                    wchar_t wc[4]; MultiByteToWideChar(CP_ACP,0,&c,1,wc,4);
                    linebuf[linelen++]=wc[0];
                }
            }
        }
    }
    if(linelen>0){
        linebuf[linelen++]=L'\r'; linebuf[linelen++]=L'\n'; linebuf[linelen]=L'\0';
        PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(linebuf));
    }

    WaitForSingleObject(pi.hProcess,INFINITE);
    DWORD ec=0; GetExitCodeProcess(pi.hProcess,&ec);
    CloseHandle(pi.hProcess); g_hprocess=NULL;
    CloseHandle(pi.hThread); CloseHandle(pr);
    return ec;
}

/* ------------------------------------------------------------------ */
/*  Single-job worker thread                                          */
/* ------------------------------------------------------------------ */
typedef struct { wchar_t cmdline[4096]; } ThreadArgs;

static DWORD WINAPI run_thread(LPVOID param)
{
    ThreadArgs *args=(ThreadArgs*)param;
    DWORD ec=run_one_job(args->cmdline);
    wchar_t done[128];
    if(ec==(DWORD)-1)
        _snwprintf(done,128,L"Failed to launch sdrtrim.exe.\r\n");
    else if(ec==0)
        _snwprintf(done,128,L"Completed successfully.\r\n");
    else
        _snwprintf(done,128,L"Failed (exit code %lu).\r\n",ec);
    PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(done));
    PostMessageW(g_hwnd,WM_APPENDLOG,1,(LPARAM)(ULONG_PTR)ec);
    free(args); return 0;
}

/* ------------------------------------------------------------------ */
/*  Batch worker thread                                               */
/* ------------------------------------------------------------------ */
static DWORD WINAPI batch_thread(LPVOID param)
{
    (void)param;
    for(int i=0;i<g_job_count;i++){
        /* Skip if already cancelled */
        if(wcscmp(g_jobs[i].status,L"Cancelled")==0) continue;

        /* Mark running */
        PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(L"Running"));

        /* Log header */
        wchar_t hdr[256];
        _snwprintf(hdr,256,L"--- Job %d of %d: %s ---\r\n",
            i+1,g_job_count,g_jobs[i].input_short);
        PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(hdr));

        /* Reset progress */
        PostMessageW(g_hwnd,WM_UPDATEPROG,0,0);

        DWORD ec=run_one_job(g_jobs[i].cmdline);

        /* If cancelled mid-job g_hprocess was terminated, ec will be non-zero */
        if(ec==(DWORD)-1 || ec!=0){
            /* Check if user cancelled — g_hprocess cleared on termination */
            const wchar_t *st=L"Failed";
            /* If the batch itself was cancelled, mark remaining jobs */
            if(!g_batch_run){
                PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(L"Cancelled"));
                /* Cancel pending jobs */
                for(int j=i+1;j<g_job_count;j++)
                    if(wcscmp(g_jobs[j].status,L"Pending")==0)
                        PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)j,(LPARAM)_wcsdup(L"Cancelled"));
                break;
            }
            PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(st));
        } else {
            PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(L"Done"));
        }

        wchar_t done[64];
        _snwprintf(done,64,L"Job %d %s.\r\n\r\n",i+1,ec==0?L"completed":L"failed");
        PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(done));
    }
    PostMessageW(g_hwnd,WM_BATCHDONE,0,0);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Segmented progress bar — owner-draw                               */
/* ------------------------------------------------------------------ */
static void draw_progress_bar(DRAWITEMSTRUCT *di)
{
    RECT r = di->rcItem;
    int w = r.right - r.left;
    int h = r.bottom - r.top;

    /* Draw to a memory DC to avoid flicker */
    HDC mdc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mdc, bmp);

    RECT mr = {0, 0, w, h};

    HBRUSH hbg = CreateSolidBrush(RGB(255,255,255));
    FillRect(mdc, &mr, hbg);
    DeleteObject(hbg);

    int seg_w=8, gap=2, pitch=seg_w+gap;
    int n_segs=(w+gap)/pitch;
    int filled=(g_progress*n_segs+50)/100;
    for(int i=0;i<n_segs;i++){
        int x=i*pitch;
        RECT sr={x, 1, x+seg_w, h-1};
        HBRUSH hb=CreateSolidBrush(i<filled?CLR_SEG_ON:CLR_SEG_OFF);
        FillRect(mdc,&sr,hb);
        DeleteObject(hb);
    }

    /* Blit to screen in one shot */
    BitBlt(di->hDC, r.left, r.top, w, h, mdc, 0, 0, SRCCOPY);

    SelectObject(mdc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mdc);
}

/* ------------------------------------------------------------------ */
/*  Coloured Run button — owner-draw                                  */
/* ------------------------------------------------------------------ */
static void draw_run_button(DRAWITEMSTRUCT *di)
{
    BOOL pressed=(di->itemState&ODS_SELECTED)!=0;
    COLORREF bg=pressed?CLR_RUN_HOT:CLR_RUN_BG;
    HBRUSH hbr=CreateSolidBrush(bg);
    FillRect(di->hDC,&di->rcItem,hbr); DeleteObject(hbr);
    HPEN hp=CreatePen(PS_SOLID,1,RGB(0,84,153));
    HPEN op=(HPEN)SelectObject(di->hDC,hp);
    HBRUSH ob=(HBRUSH)SelectObject(di->hDC,GetStockObject(NULL_BRUSH));
    RECT r=di->rcItem; r.right--; r.bottom--;
    Rectangle(di->hDC,r.left,r.top,r.right,r.bottom);
    SelectObject(di->hDC,op); SelectObject(di->hDC,ob); DeleteObject(hp);
    SetBkMode(di->hDC,TRANSPARENT);
    SetTextColor(di->hDC,CLR_RUN_TEXT);
    if(g_font_bold) SelectObject(di->hDC,g_font_bold);
    DrawTextW(di->hDC,L"Run",-1,&di->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(di->itemState&ODS_FOCUS){ RECT fr=di->rcItem; InflateRect(&fr,-3,-3); DrawFocusRect(di->hDC,&fr); }
}

/* ------------------------------------------------------------------ */
/*  Layout helpers                                                     */
/* ------------------------------------------------------------------ */
#define FH (-15)

static HWND mk_static(HWND p,const wchar_t *t,int x,int y,int w,int h)
{ HWND hw=CreateWindowExW(0,L"STATIC",t,WS_CHILD|WS_VISIBLE|SS_LEFT,x,y,w,h,p,NULL,g_hinst,NULL);
  SendMessageW(hw,WM_SETFONT,(WPARAM)g_font,FALSE); return hw; }

static HWND mk_edit(HWND p,int id,int x,int y,int w,int h,BOOL multi,BOOL ro)
{ DWORD s=WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL;
  if(multi) s|=ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_READONLY|ES_NOHIDESEL;
  if(ro) s|=ES_READONLY;
  HWND hw=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",s,x,y,w,h,p,(HMENU)(INT_PTR)id,g_hinst,NULL);
  SendMessageW(hw,WM_SETFONT,(WPARAM)(multi?g_font_mono:g_font),FALSE); return hw; }

static HWND mk_btn(HWND p,int id,const wchar_t *t,int x,int y,int w,int h,DWORD extra)
{ HWND hw=CreateWindowExW(0,L"BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|extra,
    x,y,w,h,p,(HMENU)(INT_PTR)id,g_hinst,NULL);
  SendMessageW(hw,WM_SETFONT,(WPARAM)g_font,FALSE); return hw; }

static HWND mk_radio(HWND p,int id,const wchar_t *t,int x,int y,int w,int h,BOOL first)
{ DWORD s=WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON|(first?WS_GROUP:0);
  HWND hw=CreateWindowExW(0,L"BUTTON",t,s,x,y,w,h,p,(HMENU)(INT_PTR)id,g_hinst,NULL);
  SendMessageW(hw,WM_SETFONT,(WPARAM)g_font,FALSE); return hw; }

static HWND mk_check(HWND p,int id,const wchar_t *t,int x,int y,int w,int h)
{ HWND hw=CreateWindowExW(0,L"BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX|WS_GROUP,
    x,y,w,h,p,(HMENU)(INT_PTR)id,g_hinst,NULL);
  SendMessageW(hw,WM_SETFONT,(WPARAM)g_font,FALSE); return hw; }

static HWND mk_group(HWND p,const wchar_t *t,int x,int y,int w,int h)
{ HWND hw=CreateWindowExW(0,L"BUTTON",t,WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
    x,y,w,h,p,NULL,g_hinst,NULL);
  SendMessageW(hw,WM_SETFONT,(WPARAM)g_font,FALSE); return hw; }

/* ------------------------------------------------------------------ */
/*  Create all controls                                               */
/* ------------------------------------------------------------------ */
static void create_controls(HWND hwnd)
{
    g_font      =CreateFontW(FH,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_font_bold =CreateFontW(FH,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    g_font_mono =CreateFontW(FH,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    g_font_title=CreateFontW(-20,0,0,0,FW_LIGHT,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");

    RECT cr; GetClientRect(hwnd,&cr);
    int W=cr.right, H=cr.bottom;
    int M=12, y=g_header_h+14;

    /* ── Input ── */
    mk_group(hwnd,L"Input File",M,y,W-M*2,54);
    mk_edit(hwnd,ID_INPUT_EDIT,M+10,y+22,W-M*2-86,24,FALSE,FALSE);
    mk_btn(hwnd,ID_BROWSE,L"Browse...",W-M-74,y+22,72,24,BS_PUSHBUTTON);
    y+=63;

    /* ── Mode ── */
    mk_group(hwnd,L"Mode",M,y,W-M*2,54);
    mk_radio(hwnd,ID_MODE_FULL,L"Full file",M+10, y+26,84,20,TRUE);
    mk_radio(hwnd,ID_MODE_TRIM,L"Trim",     M+102,y+26,58,20,FALSE);
    mk_static(hwnd,L"Start:",M+170,y+28,42,16);
    mk_edit(hwnd,ID_START_EDIT,M+214,y+24,52,24,FALSE,FALSE);
    mk_static(hwnd,L"End:",M+274,y+28,34,16);
    mk_edit(hwnd,ID_END_EDIT,M+310,y+24,52,24,FALSE,FALSE);
    mk_static(hwnd,L"HHMM",M+368,y+28,52,16);
    y+=63;

    /* ── Channel ── */
    mk_group(hwnd,L"Channel",M,y,W-M*2,50);
    mk_radio(hwnd,ID_CHAN_DUAL,L"Dual (both tuners)",M+10, y+26,148,20,TRUE);
    mk_radio(hwnd,ID_CHAN_CH1, L"Tuner A  (--ch1)",  M+174,y+26,130,20,FALSE);
    mk_radio(hwnd,ID_CHAN_CH2, L"Tuner B  (--ch2)",  M+318,y+26,130,20,FALSE);
    y+=59;

    /* ── DDC ── */
    mk_group(hwnd,L"DDC  (Digital Down-Conversion)",M,y,W-M*2,54);
    mk_check(hwnd,ID_DDC_CHECK,L"Enable",M+10,y+26,72,20);
    mk_static(hwnd,L"Centre:",M+92, y+28,52,16);
    mk_edit(hwnd,ID_DDC_CENTRE,M+146,y+24,68,24,FALSE,FALSE);
    mk_static(hwnd,L"kHz",M+218,y+28,30,16);
    mk_static(hwnd,L"BW:",M+260,y+28,30,16);
    mk_edit(hwnd,ID_DDC_BW,M+292,y+24,68,24,FALSE,FALSE);
    mk_static(hwnd,L"kHz",M+364,y+28,30,16);
    y+=63;

    /* ── Output format ── */
    mk_group(hwnd,L"Output Format",M,y,W-M*2,50);
    mk_radio(hwnd,ID_FMT_SAME,      L"Same as input",M+10, y+26,118,20,TRUE);
    mk_radio(hwnd,ID_FMT_LINRAD,    L"linrad",        M+142,y+26, 72,20,FALSE);
    mk_radio(hwnd,ID_FMT_WAVVIEWDX, L"wavviewdx",     M+228,y+26, 96,20,FALSE);
    mk_radio(hwnd,ID_FMT_SDRUNO,    L"sdruno",         M+338,y+26, 72,20,FALSE);
    mk_radio(hwnd,ID_FMT_SDRCONNECT,L"sdrconnect",    M+424,y+26, 96,20,FALSE);
    y+=59;

    /* ── Predicted output label ── */
    HWND hpred=mk_static(hwnd,L"Output: (select input file)",M+2,y,W-M*2-4,18);
    SetWindowLongW(hpred,GWLP_ID,ID_OUTFILE_STATIC);
    y+=26;

    /* ── Log ── */
    int log_h=120;
    { HWND hg=mk_group(hwnd,L"Output Log",M,y,W-M*2,log_h+22);
      SetWindowLongW(hg,GWLP_ID,ID_LOG_GROUP); }
    mk_edit(hwnd,ID_LOG_EDIT,M+8,y+20,W-M*2-16,log_h,TRUE,TRUE);
    y+=log_h+30;

    /* ── Progress bar row ── */
    int prog_h=18, lbl_h=18;
    int lbl_y=y+(prog_h-lbl_h)/2;
    HWND hpct=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_RIGHT,
        M,lbl_y,36,lbl_h,hwnd,(HMENU)(INT_PTR)ID_PCT_STATIC,g_hinst,NULL);
    SendMessageW(hpct,WM_SETFONT,(WPARAM)g_font,FALSE);
    HWND heta=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,
        W-M-90,lbl_y,90,lbl_h,hwnd,(HMENU)(INT_PTR)ID_ETA_STATIC,g_hinst,NULL);
    SendMessageW(heta,WM_SETFONT,(WPARAM)g_font,FALSE);
    int bar_x=M+40, bar_w=W-M-94-bar_x;
    CreateWindowExW(WS_EX_CLIENTEDGE,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,
        bar_x,y,bar_w,prog_h,hwnd,(HMENU)(INT_PTR)ID_PROGRESS,g_hinst,NULL);
    y+=prog_h+8;

    /* ── Buttons row ── */
    mk_btn(hwnd,ID_RUN,    L"Run",       W-M-256,y,78,28,BS_OWNERDRAW);
    mk_btn(hwnd,ID_CANCEL, L"Cancel",    W-M-170,y,78,28,BS_PUSHBUTTON);
    mk_btn(hwnd,ID_CLEAR,  L"Clear Log", W-M-84, y,78,28,BS_PUSHBUTTON);
    y+=38;

    /* ── Batch section ── */
    /* Separator label */
    mk_static(hwnd,L"Batch Jobs",M,y+4,90,18);
    mk_btn(hwnd,ID_ADD_BATCH,  L"Add to Batch",W-M-274,y,90,26,BS_PUSHBUTTON);
    mk_btn(hwnd,ID_RUN_BATCH,  L"Run Batch",   W-M-176,y,86,26,BS_PUSHBUTTON);
    mk_btn(hwnd,ID_CLEAR_BATCH,L"Clear All",   W-M-82, y,70,26,BS_PUSHBUTTON);
    y+=34;

    /* ── Batch ListView ── */
    int lv_h=H-M-y;
    if(lv_h<80) lv_h=80;
    HWND hlv=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|
        LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_NOSORTHEADER,
        M,y,W-M*2,lv_h,
        hwnd,(HMENU)(INT_PTR)ID_BATCH_LIST,g_hinst,NULL);
    SendMessageW(hlv,WM_SETFONT,(WPARAM)g_font,FALSE);
    ListView_SetExtendedListViewStyle(hlv,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);

    /* Columns */
    LVCOLUMNW lc; ZeroMemory(&lc,sizeof(lc)); lc.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
    /* Default column widths — overridden by saved values below */
    int def_cw[]={30,140,170,140,80};
    /* Load saved widths from INI if available */
    wchar_t ini_pth[MAX_PATH]; ini_path(ini_pth,MAX_PATH);
    for(int col=0;col<5;col++){
        wchar_t key[16]; _snwprintf(key,16,L"col%d",col);
        int saved=(int)GetPrivateProfileIntW(L"Cols",key,-1,ini_pth);
        if(saved>10) def_cw[col]=saved;
    }
    const wchar_t *col_names[]={L"#",L"Input",L"Operation",L"Output",L"Status"};
    int col_ids[]={COL_NUM,COL_INPUT,COL_OP,COL_OUTPUT,COL_STATUS};
    for(int col=0;col<5;col++){
        lc.cx=def_cw[col]; lc.pszText=(wchar_t*)col_names[col];
        lc.iSubItem=col_ids[col];
        ListView_InsertColumn(hlv,col_ids[col],&lc);
    }

    /* ── Initial state ── */
    CheckRadioButton(hwnd,ID_MODE_FULL,ID_MODE_TRIM,ID_MODE_FULL);
    EnableWindow(GetDlgItem(hwnd,ID_START_EDIT),FALSE);
    EnableWindow(GetDlgItem(hwnd,ID_END_EDIT),  FALSE);
    CheckRadioButton(hwnd,ID_CHAN_DUAL,ID_CHAN_CH2,ID_CHAN_DUAL);
    CheckRadioButton(hwnd,ID_FMT_SAME,ID_FMT_SDRCONNECT,ID_FMT_SAME);
    EnableWindow(GetDlgItem(hwnd,ID_DDC_CENTRE),FALSE);
    EnableWindow(GetDlgItem(hwnd,ID_DDC_BW),    FALSE);
    EnableWindow(GetDlgItem(hwnd,ID_CANCEL),    FALSE);

    /* Header panel — created LAST so it has highest z-order */
    CreateWindowExW(0,L"BUTTON",L"",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        0,0,W,g_header_h,hwnd,(HMENU)(INT_PTR)ID_HEADER_PANEL,g_hinst,NULL);
}

/* ------------------------------------------------------------------ */
/*  Browse for input file                                             */
/* ------------------------------------------------------------------ */
static void browse_file(void)
{
    wchar_t buf[MAX_PATH]={0};
    OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof(ofn));
    ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=g_hwnd;
    ofn.lpstrFilter=L"IQ Recordings\0*.raw;*.wav\0All Files\0*.*\0";
    ofn.lpstrFile=buf; ofn.nMaxFile=MAX_PATH;
    ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    ofn.lpstrTitle=L"Select Input Recording";
    if(GetOpenFileNameW(&ofn)){
        SetWindowTextW(GetDlgItem(g_hwnd,ID_INPUT_EDIT),buf);
        update_channel_controls();
        update_format_controls();
        update_prediction();
    }
}

/* ------------------------------------------------------------------ */
/*  Window procedure                                                  */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK wnd_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)
{
    switch(msg){

    case WM_CREATE:
        g_hwnd=hwnd;
        g_hbr_bg    =CreateSolidBrush(CLR_BG);
        g_hbr_header=CreateSolidBrush(CLR_HEADER_BG);
        return 0;


    case WM_GETMINMAXINFO:{
        MINMAXINFO *mm=(MINMAXINFO*)lp;
        mm->ptMinTrackSize.x=660;
        mm->ptMinTrackSize.y=700;
        return 0;}

    case WM_SIZE:{
        static BOOL done=FALSE;
        if(!done){
            done=TRUE;
            create_controls(hwnd);
            restore_settings();
            update_prediction();
            return 0;
        }
        /* Resize: reposition/resize stretchy controls */
        if(wp==SIZE_MINIMIZED) return 0;
        int W=LOWORD(lp), H=HIWORD(lp);
        int M=12;
        HDWP hdwp=BeginDeferWindowPos(16);
        #define MV(id,x,y,w,h) \
            hdwp=DeferWindowPos(hdwp,GetDlgItem(hwnd,id),NULL,x,y,w,h, \
                SWP_NOZORDER|SWP_NOACTIVATE);

        /* Input edit stretches, Browse stays right */
        MV(ID_INPUT_EDIT, M+10,     g_header_h+14+22,  W-M*2-86,  24)
        MV(ID_BROWSE,     W-M-74,   g_header_h+14+22,  72,        24)

        /* Prediction label stretches */
        MV(ID_OUTFILE_STATIC, M+2,  373,  W-M*2-4, 18)

        /* Log group + edit stretch */
        int log_top = 399;
        int log_group_h = 120;
        MV(ID_LOG_EDIT,   M+8,      log_top+20, W-M*2-16, log_group_h)
        MV(ID_LOG_GROUP,  M,        log_top,    W-M*2,    log_group_h+22)

        /* Progress bar stretches */
        int prog_y=log_top+log_group_h+30;
        int bar_x=M+40, bar_w=W-M-94-bar_x;
        MV(ID_PROGRESS,  bar_x, prog_y,       bar_w, 18)
        MV(ID_ETA_STATIC,W-M-90,prog_y,       90,    18)

        /* Buttons move right */
        int btn_y=prog_y+26;
        MV(ID_RUN,    W-M-256, btn_y, 78, 28)
        MV(ID_CANCEL, W-M-170, btn_y, 78, 28)
        MV(ID_CLEAR,  W-M-84,  btn_y, 78, 28)

        /* Batch toolbar buttons move right */
        int batch_hdr_y=btn_y+38;
        MV(ID_ADD_BATCH,   W-M-274, batch_hdr_y, 90, 26)
        MV(ID_RUN_BATCH,   W-M-176, batch_hdr_y, 86, 26)
        MV(ID_CLEAR_BATCH, W-M-82,  batch_hdr_y, 70, 26)

        /* Batch list stretches both ways */
        int lv_y=batch_hdr_y+34;
        int lv_h=H-M-lv_y;
        if(lv_h<80) lv_h=80;
        MV(ID_BATCH_LIST, M, lv_y, W-M*2, lv_h)

        /* Header panel stretches */
        MV(ID_HEADER_PANEL, 0, 0, W, g_header_h)

        #undef MV
        EndDeferWindowPos(hdwp);
        return 0;}

    case WM_ERASEBKGND:{
        HDC dc=(HDC)wp; RECT r; GetClientRect(hwnd,&r);
        FillRect(dc,&r,g_hbr_bg);
        return 1;}

    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp,RGB(255,255,255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);

    case WM_CTLCOLORSTATIC:{
        wchar_t cls[16]; GetClassNameW((HWND)lp,cls,16);
        if(_wcsicmp(cls,L"EDIT")==0){
            SetBkColor((HDC)wp,RGB(240,240,240));
            return (LRESULT)GetStockObject(LTGRAY_BRUSH);
        }
        SetBkColor((HDC)wp,CLR_BG);
        return (LRESULT)g_hbr_bg;}

    case WM_CTLCOLORBTN:
        if((HWND)lp==GetDlgItem(hwnd,ID_RUN)) return (LRESULT)NULL;
        SetBkColor((HDC)wp,CLR_BG);
        return (LRESULT)g_hbr_bg;

    case WM_DRAWITEM:{
        DRAWITEMSTRUCT *di=(DRAWITEMSTRUCT*)lp;
        if(di->CtlID==ID_RUN)     { draw_run_button(di);   return TRUE; }
        if(di->CtlID==ID_PROGRESS){ draw_progress_bar(di); return TRUE; }
        if(di->CtlID==ID_HEADER_PANEL){
            FillRect(di->hDC,&di->rcItem,g_hbr_header);
            SetBkMode(di->hDC,TRANSPARENT);
            SetTextColor(di->hDC,CLR_HEADER_TEXT);
            if(g_font_title) SelectObject(di->hDC,g_font_title);
            RECT tr=di->rcItem; tr.left+=14;
            DrawTextW(di->hDC,L"SDR Trim  v1.4",-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            /* Name on right — use normal font, slightly smaller */
            if(g_font) SelectObject(di->hDC,g_font);
            RECT nr=di->rcItem; nr.right-=14;
            DrawTextW(di->hDC,L"github.com/45south",-1,&nr,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            return TRUE;
        }
        break;}

    case WM_COMMAND:
        switch(LOWORD(wp)){
        case ID_BROWSE: browse_file(); break;
        case ID_MODE_FULL:
            EnableWindow(GetDlgItem(hwnd,ID_START_EDIT),FALSE);
            EnableWindow(GetDlgItem(hwnd,ID_END_EDIT),  FALSE);
            update_format_controls(); update_prediction(); break;
        case ID_MODE_TRIM:
            EnableWindow(GetDlgItem(hwnd,ID_START_EDIT),TRUE);
            EnableWindow(GetDlgItem(hwnd,ID_END_EDIT),  TRUE);
            SetFocus(GetDlgItem(hwnd,ID_START_EDIT));
            update_format_controls(); update_prediction(); break;
        case ID_CHAN_DUAL: case ID_CHAN_CH1: case ID_CHAN_CH2:
        case ID_FMT_SAME: case ID_FMT_LINRAD: case ID_FMT_WAVVIEWDX:
        case ID_FMT_SDRUNO: case ID_FMT_SDRCONNECT:
            update_format_controls(); update_prediction(); break;
        case ID_DDC_CHECK:{
            BOOL en=IsDlgButtonChecked(hwnd,ID_DDC_CHECK);
            EnableWindow(GetDlgItem(hwnd,ID_DDC_CENTRE),en);
            EnableWindow(GetDlgItem(hwnd,ID_DDC_BW),    en);
            update_format_controls(); update_prediction(); break;}
        case ID_DDC_CENTRE: case ID_DDC_BW:
        case ID_START_EDIT: case ID_END_EDIT: case ID_INPUT_EDIT:
            if(HIWORD(wp)==EN_CHANGE){ update_channel_controls(); update_format_controls(); update_prediction(); } break;

        case ID_RUN:
            if(g_running) break;
            {
                wchar_t input[MAX_PATH];
                GetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),input,MAX_PATH);
                wchar_t outfile[MAX_PATH]={0};
                if(predict_outfile(input,outfile,MAX_PATH)){
                    if(GetFileAttributesW(outfile)!=INVALID_FILE_ATTRIBUTES){
                        wchar_t msg[MAX_PATH+80];
                        wchar_t *fn=wcsrchr(outfile,L'\\');
                        _snwprintf(msg,MAX_PATH+80,L"Output file already exists:\n\n%s\n\nOverwrite?",fn?fn+1:outfile);
                        if(MessageBoxW(hwnd,msg,L"SDR Trim - Overwrite?",
                                MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) break;
                    }
                }
                ThreadArgs *args=(ThreadArgs*)malloc(sizeof(ThreadArgs));
                if(!args) break;
                if(!build_cmdline(args->cmdline,4096)){ free(args); break; }
                save_settings();
                log_clear();
                SendDlgItemMessageW(hwnd,ID_PROGRESS,0,0,0); /* trigger redraw at 0 */
                g_progress=0; InvalidateRect(GetDlgItem(hwnd,ID_PROGRESS),NULL,FALSE);
                wchar_t hdr[4096+32]; _snwprintf(hdr,4096+32,L"> %s\r\n\r\n",args->cmdline);
                log_append(hdr);
                g_running=TRUE;
                g_batch_run=FALSE;
                EnableWindow(GetDlgItem(hwnd,ID_RUN),FALSE);
                EnableWindow(GetDlgItem(hwnd,ID_CANCEL),TRUE);
                SetWindowTextW(GetDlgItem(hwnd,ID_PCT_STATIC),L"");
                SetWindowTextW(GetDlgItem(hwnd,ID_ETA_STATIC),L"");
                g_thread=CreateThread(NULL,0,run_thread,args,0,NULL);
                if(!g_thread){
                    g_running=FALSE;
                    EnableWindow(GetDlgItem(hwnd,ID_RUN),TRUE);
                    EnableWindow(GetDlgItem(hwnd,ID_CANCEL),FALSE);
                    free(args);
                }
            }
            break;

        case ID_CANCEL:
            if(g_running&&g_hprocess){
                g_batch_run=FALSE;  /* signal batch to stop */
                TerminateProcess(g_hprocess,1);
                log_append(L"Cancelled by user.\r\n");
            }
            break;

        case ID_CLEAR: log_clear(); break;

        case ID_ADD_BATCH:
            if(g_job_count>=MAX_JOBS){
                MessageBoxW(hwnd,L"Batch list is full (64 jobs maximum).",
                    L"SDR Trim",MB_OK|MB_ICONWARNING); break;
            }
            {
                BatchJob *j=&g_jobs[g_job_count];
                if(!build_cmdline(j->cmdline,4096)) break;
                build_op_desc(j->op_desc,128);
                wcsncpy(j->status,L"Pending",31);
                /* Input short name */
                wchar_t input[MAX_PATH];
                GetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),input,MAX_PATH);
                wchar_t *fn=wcsrchr(input,L'\\');
                wcsncpy(j->input_short,fn?fn+1:input,63);
                /* Output short name */
                wchar_t outfile[MAX_PATH]={0};
                if(predict_outfile(input,outfile,MAX_PATH)){
                    wchar_t *of=wcsrchr(outfile,L'\\');
                    wcsncpy(j->output_short,of?of+1:outfile,63);
                    wcsncpy(j->output_full, outfile, MAX_PATH-1);
                } else {
                    wcsncpy(j->output_short,L"(unknown)",63);
                    j->output_full[0]=L'\0';
                }
                batch_list_add_row(g_job_count);
                g_job_count++;
                update_run_batch_button();
            }
            break;

        case ID_RUN_BATCH:
            if(g_running||g_job_count==0) break;
            {
                /* Check for output files that already exist */
                wchar_t conflict_list[4096]={0};
                int conflict_count=0;
                for(int i=0;i<g_job_count;i++){
                    if(wcscmp(g_jobs[i].status,L"Done")==0) continue;
                    if(wcscmp(g_jobs[i].status,L"Cancelled")==0) continue;
                    if(g_jobs[i].output_full[0] &&
                       GetFileAttributesW(g_jobs[i].output_full)!=INVALID_FILE_ATTRIBUTES){
                        conflict_count++;
                        if(wcslen(conflict_list)<3900){
                            wchar_t num[8]; _snwprintf(num,8,L"%d",i+1);
                            wcsncat(conflict_list,L"  Job ",4095-wcslen(conflict_list));
                            wcsncat(conflict_list,num,4095-wcslen(conflict_list));
                            wcsncat(conflict_list,L": ",4095-wcslen(conflict_list));
                            wcsncat(conflict_list,g_jobs[i].output_short,4095-wcslen(conflict_list));
                            wcsncat(conflict_list,L"\n",4095-wcslen(conflict_list));
                        }
                    }
                }
                if(conflict_count>0){
                    wchar_t msg[4096+128];
                    _snwprintf(msg,4096+128,
                        L"%d output file(s) already exist and will be overwritten:\n\n%s\nContinue?",
                        conflict_count,conflict_list);
                    if(MessageBoxW(hwnd,msg,L"SDR Trim Batch - Overwrite?",
                            MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) break;
                }
                /* Reset all Pending/Cancelled jobs */
                for(int i=0;i<g_job_count;i++)
                    if(wcscmp(g_jobs[i].status,L"Pending")==0||
                       wcscmp(g_jobs[i].status,L"Cancelled")==0)
                        batch_list_set_status(i,L"Pending");
                log_clear();
                g_progress=0; InvalidateRect(GetDlgItem(hwnd,ID_PROGRESS),NULL,FALSE);
                g_running=TRUE;
                g_batch_run=TRUE;
                EnableWindow(GetDlgItem(hwnd,ID_RUN),FALSE);
                EnableWindow(GetDlgItem(hwnd,ID_RUN_BATCH),FALSE);
                EnableWindow(GetDlgItem(hwnd,ID_CANCEL),TRUE);
                SetWindowTextW(GetDlgItem(hwnd,ID_PCT_STATIC),L"");
                SetWindowTextW(GetDlgItem(hwnd,ID_ETA_STATIC),L"");
                g_thread=CreateThread(NULL,0,batch_thread,NULL,0,NULL);
            }
            break;

        case ID_CLEAR_BATCH:
            if(g_running) break;
            g_job_count=0;
            ListView_DeleteAllItems(GetDlgItem(hwnd,ID_BATCH_LIST));
            update_run_batch_button();
            break;
        }
        return 0;

    /* Right-click context menu on batch list */
    case WM_NOTIFY:{
        NMHDR *nm=(NMHDR*)lp;
        if(nm->idFrom==ID_BATCH_LIST&&nm->code==NM_RCLICK){
            NMITEMACTIVATE *nia=(NMITEMACTIVATE*)lp;
            int idx=nia->iItem;
            if(idx>=0&&idx<g_job_count&&!g_running){
                POINT pt; GetCursorPos(&pt);
                HMENU hm=CreatePopupMenu();
                AppendMenuW(hm,MF_STRING,1,L"Remove job");
                int cmd=TrackPopupMenu(hm,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,hwnd,NULL);
                DestroyMenu(hm);
                if(cmd==1){
                    /* Remove job at idx, shift rest down */
                    for(int i=idx;i<g_job_count-1;i++) g_jobs[i]=g_jobs[i+1];
                    g_job_count--;
                    ListView_DeleteItem(GetDlgItem(hwnd,ID_BATCH_LIST),idx);
                    /* Renumber remaining rows */
                    for(int i=idx;i<g_job_count;i++) batch_list_refresh_row(i);
                }
            }
        }
        break;}

    case WM_UPDATEPROG:{
        int pct=(int)(WPARAM)wp;
        if(pct!=g_progress){
            g_progress=pct;
            InvalidateRect(GetDlgItem(hwnd,ID_PROGRESS),NULL,FALSE);
        }
        wchar_t ps[8]; _snwprintf(ps,8,L"%d%%",pct);
        SetWindowTextW(GetDlgItem(hwnd,ID_PCT_STATIC),ps);
        return 0;}

    case WM_UPDATEETA:
        if(lp){ SetWindowTextW(GetDlgItem(hwnd,ID_ETA_STATIC),(wchar_t*)lp); free((void*)lp); }
        return 0;

    case WM_APPENDLOG:
        if(wp==0&&lp){ log_append((wchar_t*)lp); free((void*)lp); }
        else if(wp==1){
            g_running=FALSE; g_hprocess=NULL;
            EnableWindow(GetDlgItem(hwnd,ID_RUN),TRUE);
            EnableWindow(GetDlgItem(hwnd,ID_CANCEL),FALSE);
            SetWindowTextW(GetDlgItem(hwnd,ID_ETA_STATIC),L"");
            DWORD ec=(DWORD)(ULONG_PTR)lp;
            g_progress=(ec==0?100:0);
            InvalidateRect(GetDlgItem(hwnd,ID_PROGRESS),NULL,FALSE);
            InvalidateRect(hwnd,NULL,FALSE);
            if(g_thread){ CloseHandle(g_thread); g_thread=NULL; }
        }
        return 0;

    case WM_BATCHSTATUS:
        if(lp){
            int idx=(int)wp;
            batch_list_set_status(idx,(wchar_t*)lp);
            free((void*)lp);
            update_run_batch_button();
        }
        return 0;

    case WM_BATCHDONE:
        g_running=FALSE; g_batch_run=FALSE; g_hprocess=NULL;
        EnableWindow(GetDlgItem(hwnd,ID_RUN),TRUE);
        EnableWindow(GetDlgItem(hwnd,ID_CANCEL),FALSE);
        update_run_batch_button();
        SetWindowTextW(GetDlgItem(hwnd,ID_ETA_STATIC),L"");
        log_append(L"Batch complete.\r\n");
        InvalidateRect(hwnd,NULL,FALSE);
        if(g_thread){ CloseHandle(g_thread); g_thread=NULL; }
        return 0;

    case WM_DROPFILES:{
        wchar_t path[MAX_PATH];
        DragQueryFileW((HDROP)wp,0,path,MAX_PATH);
        SetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),path);
        DragFinish((HDROP)wp);
        update_channel_controls();
        update_format_controls();
        update_prediction();
        return 0;}

    case WM_CLOSE:{
        /* Save window size */
        RECT wr; GetWindowRect(hwnd,&wr);
        ini_wi(L"W",L"cx",wr.right-wr.left);
        ini_wi(L"W",L"cy",wr.bottom-wr.top);
        /* Save ListView column widths */
        HWND hlv=GetDlgItem(hwnd,ID_BATCH_LIST);
        for(int col=0;col<5;col++){
            int cw=ListView_GetColumnWidth(hlv,col);
            wchar_t key[16]; _snwprintf(key,16,L"col%d",col);
            ini_wi(L"Cols",key,cw);
        }
        save_settings();
        DestroyWindow(hwnd);}
        return 0;

    case WM_DESTROY:
        if(g_font)       DeleteObject(g_font);
        if(g_font_bold)  DeleteObject(g_font_bold);
        if(g_font_mono)  DeleteObject(g_font_mono);
        if(g_font_title) DeleteObject(g_font_title);
        if(g_hbr_bg)     DeleteObject(g_hbr_bg);
        if(g_hbr_header) DeleteObject(g_hbr_header);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

/* ------------------------------------------------------------------ */
/*  WinMain                                                           */
/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE hp,LPWSTR lp,int ns)
{
    (void)hp;(void)lp;
    g_hinst=hi;
    GetModuleFileNameW(NULL,g_exedir,MAX_PATH);
    wchar_t *sl=wcsrchr(g_exedir,L'\\'); if(sl)*sl=L'\0';

    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc={sizeof(icc),
        ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES|ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc; ZeroMemory(&wc,sizeof(wc));
    wc.cbSize=sizeof(wc); wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=wnd_proc; wc.hInstance=hi;
    wc.hIcon=LoadIconW(NULL,IDI_APPLICATION);
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    wc.lpszClassName=L"SDRTrimGUI";
    RegisterClassExW(&wc);

    int cw=640, ch=900;
    RECT rc={0,0,cw,ch};
    AdjustWindowRect(&rc,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_THICKFRAME,FALSE);
    int ww=rc.right-rc.left, wh=rc.bottom-rc.top;
    int sx=(GetSystemMetrics(SM_CXSCREEN)-ww)/2;
    int sy=(GetSystemMetrics(SM_CYSCREEN)-wh)/2;

    /* Restore saved window size if available */
    wchar_t ini_p[MAX_PATH]; ini_path(ini_p,MAX_PATH);
    int saved_cx=(int)GetPrivateProfileIntW(L"W",L"cx",0,ini_p);
    int saved_cy=(int)GetPrivateProfileIntW(L"W",L"cy",0,ini_p);
    if(saved_cx>=660&&saved_cy>=700){ ww=saved_cx; wh=saved_cy;
        sx=(GetSystemMetrics(SM_CXSCREEN)-ww)/2;
        sy=(GetSystemMetrics(SM_CYSCREEN)-wh)/2; }
    HWND hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"SDRTrimGUI",L"SDR Trim",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_THICKFRAME|WS_MAXIMIZEBOX,
        sx,sy,ww,wh,NULL,NULL,hi,NULL);

    ShowWindow(hwnd,ns); UpdateWindow(hwnd);
    MSG m;
    while(GetMessageW(&m,NULL,0,0)){ TranslateMessage(&m); DispatchMessageW(&m); }
    return (int)m.wParam;
}
