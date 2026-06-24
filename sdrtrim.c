/*
 * sdrtrim.c  —  SDR Trim  —  self-contained Win32 GUI + processing engine
 *
 * Build (MSYS2/MinGW64):
 *   gcc -Wall -O2 -o sdrtrim.exe sdrtrim.c sdrtrim.res \
 *       -lcomctl32 -lcomdlg32 -lshell32 -mwindows -municode
 *
 * Single executable — no external dependencies.
 * Settings saved to sdrtrim.ini in the same folder.
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
#include <shlobj.h>
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
#define ID_FMT_PERSEUS      140
#define ID_FMT_JAGUAR       141
#define ID_OUT_16BIT        142
#define ID_OUT_24BIT        143
#define ID_BIT_LABEL        144
#define ID_OUTFOLDER_EDIT   145
#define ID_OUTFOLDER_BROWSE 146
#define ID_OUTFOLDER_CLEAR  147
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
#define ID_REC_INFO         127
#define ID_SR_INFO          128
#define ID_SEQ_ADD          129
#define SEQ_MAX             16   /* max files in sequence */
#define ID_SEQ_LIST         134
#define ID_SEQ_REMOVE       135
#define ID_DDC_UNAVAIL      136
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
#define WM_CONFIRM_OW   (WM_USER + 6)  /* wp=0 lp=heap wchar_t* path; returns IDYES/IDNO */

/* ================================================================== */
/*  Processing engine shim — bridges sdrtrim core to GUI              */
/* ================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <time.h>

static HWND g_proc_hwnd = NULL;  /* set before each job */

static void proc_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    int n=(int)strlen(buf);
    /* Convert \n to \r\n for edit control */
    wchar_t wbuf[2048]; int wi=0;
    for(int i=0;i<n&&wi<2045;i++){
        if(buf[i]=='\n'){ wbuf[wi++]=L'\r'; wbuf[wi++]=L'\n'; }
        else { wbuf[wi++]=(wchar_t)(unsigned char)buf[i]; }
    }
    wbuf[wi]=L'\0';
    wchar_t *heap=_wcsdup(wbuf);
    if(heap) PostMessageW(g_proc_hwnd,WM_APPENDLOG,0,(LPARAM)heap);
}

static void proc_perror(const char *msg)
{
    proc_log("%s: %s\n", msg, strerror(errno));
}

/* ================================================================== */
/*  sdrtrim processing core (from sdrtrim.c)                          */
/* ================================================================== */
/*
 * sdrtrim.c
 *
 * SDR Trim — IQ Recording Trim, Convert and Frequency-Extract Utility
 *
 * Reads SDR IQ recordings in any supported input format, optionally
 * trims to a start/end time, optionally applies DDC frequency extraction,
 * and writes the result in any compatible output format.
 *
 * ── Supported input formats (auto-detected) ──────────────────────────
 *
 *   Linrad raw     (.raw)  41-byte header, single or dual tuner
 *   WavViewDX raw  (.raw)  headerless, metadata in filename
 *   SDRuno WAV     (.wav)  RIFF + auxi chunk, single tuner
 *   SDR Connect    (.wav)  RIFF + JUNK chunk, single tuner
 *
 * ── Supported output formats ─────────────────────────────────────────
 *
 *   Dual-tuner output:    Linrad raw, WavViewDX raw
 *   Single-tuner output:  Linrad raw, WavViewDX raw, SDRuno WAV,
 *                         SDR Connect WAV
 *
 * ── Usage ────────────────────────────────────────────────────────────
 *
 *   Trim:
 *     sdrtrim <input> <start_HHMM> <end_HHMM> [options]
 *
 *   Full-file (no trim):
 *     sdrtrim <input> [options]
 *     sdrtrim <input> --convert [options]   (--convert accepted for compatibility)
 *
 *   Options:
 *     --ch1              Extract tuner A as single-tuner output
 *     --ch2              Extract tuner B as single-tuner output
 *     linrad|wavviewdx|sdruno|sdrconnect
 *                        Output format (bare word, no switch needed).
 *                        Default: same as input format.
 *     --ddc <kHz> <bw>   DDC frequency extraction (centre kHz, bandwidth kHz)
 *
 * ── Examples ─────────────────────────────────────────────────────────
 *
 *   sdrtrim rec.raw 033000 150000
 *   sdrtrim rec.raw 033000 150000 --ch1 sdruno
 *   sdrtrim rec.raw 033000 150000 --ch2 sdrconnect
 *   sdrtrim rec.raw 033000 150000 wavviewdx
 *   sdrtrim rec.raw 033000 150000 --ddc 1044 500 --ch1 sdrconnect
 *   sdrtrim rec.raw --ch1 sdruno
 *   sdrtrim rec.raw --ch1
 *   sdrtrim rec.raw wavviewdx
 *
 * ── Build ─────────────────────────────────────────────────────────────
 *   Linux/macOS : gcc -O2 -Wall -o sdrtrim sdrtrim.c -lm
 *   MSYS2/MinGW : gcc -Wall -o sdrtrim.exe sdrtrim.c sdrtrim.res -lcomctl32 -lcomdlg32 -lshell32 -mwindows -municode -lm
 */

#define _FILE_OFFSET_BITS 64

/* ------------------------------------------------------------------ */
/*  Platform: 64-bit seek/tell                                         */
/* ------------------------------------------------------------------ */
#if defined(_WIN32) || defined(_WIN64)
#  include <io.h>
#  define file_seek(fp,off,wh)  _fseeki64((fp),(off),(wh))
#  define file_tell(fp)         _ftelli64(fp)
#else
#  define file_seek(fp,off,wh)  fseeko((fp),(off_t)(off),(wh))
#  define file_tell(fp)         (int64_t)ftello(fp)
#endif

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/*  Enumerations                                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    FMT_LINRAD    = 0,
    FMT_WAVVIEWDX = 1,
    FMT_SDRUNO    = 2,
    FMT_SDRCONNECT= 3,
    FMT_PERSEUS   = 4,
    FMT_JAGUAR    = 5,
    FMT_UNKNOWN   = 6
} FileFormat;

/* rcvr chunk as used by Perseus (flags=4) and Jaguar (flags=5).
   Must be packed to match the 34-byte on-disk layout exactly. */
typedef struct __attribute__((packed)) {
    uint32_t centre_freq_hz;
    uint32_t flags;           /* 4=Perseus, 5=Jaguar */
    uint32_t unix_timestamp;
    uint8_t  padding[22];     /* preserve verbatim */
} RcvrChunk;
typedef enum {
    TUNER_SINGLE = 0,   /* 2 interleaved int16 per frame: I, Q          */
    TUNER_DUAL   = 1    /* 4 interleaved int16 per frame: IA,QA,IB,QB   */
} TunerMode;

typedef enum {
    OUT_DUAL = 0,       /* keep both tuner channels                      */
    OUT_CH1  = 1,       /* tuner A only (or only channel for single)     */
    OUT_CH2  = 2        /* tuner B only (dual input only)                */
} OutputChan;

/* ------------------------------------------------------------------ */
/*  Format-agnostic recording metadata                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    FileFormat  fmt;
    TunerMode   tuner;
    int         sample_rate;        /* Hz                               */
    int         centre_freq_hz;     /* Hz                               */
    int         num_channels;       /* 2 (single) or 4 (dual)           */
    int         bits_per_sample;    /* 16 or 24 (Perseus lower rates)   */
    int64_t     epoch;              /* Unix timestamp of recording start */
    int         year, month, day;
    int         hour, minute, second;
    int         utc_offset;         /* Linrad UTC digit (0 for others)  */
    int64_t     data_offset;        /* byte offset of first sample      */
    uint64_t    data_bytes;         /* total sample data bytes          */
    char        filepath[512];
    RcvrChunk   rcvr;               /* preserved verbatim for Perseus/Jaguar output */
} RecInfo;

/* ------------------------------------------------------------------ */
/*  Packed WAV / Linrad on-disk structures                             */
/* ------------------------------------------------------------------ */
#pragma pack(push,1)

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;   /* 16 bytes */

/* SDRuno auxi chunk  -  164 bytes confirmed from real files */
typedef struct {
    uint16_t start_year, start_month, start_dow, start_day;
    uint16_t start_hour, start_minute, start_second, start_ms;
    uint16_t stop_year,  stop_month,  stop_dow,  stop_day;
    uint16_t stop_hour,  stop_minute,  stop_second,  stop_ms;
    int32_t  centre_freq_hz;
    int32_t  afc_offset;
    int32_t  if_frequency;
    int32_t  bandwidth;
    int32_t  if_bandwidth;
    int32_t  demod_mode;
    char     demod_name[16];
    int32_t  unused1;
    int32_t  unused2;
    char     filename[84];
} AuxiChunk;   /* 164 bytes */

typedef struct {
    uint64_t riff_size;
    uint64_t data_size;
    uint64_t sample_count;
    uint32_t table_len;
} Ds64Payload;   /* 28 bytes */

#pragma pack(pop)

typedef char _chk_fmt [sizeof(FmtChunk)    == 16  ? 1 : -1];
typedef char _chk_auxi[sizeof(AuxiChunk)   == 164 ? 1 : -1];
typedef char _chk_ds64[sizeof(Ds64Payload) == 28  ? 1 : -1];

/* Linrad raw header size (handled via explicit byte I/O) */
#define LINRAD_HDR_SIZE 41

/* ------------------------------------------------------------------ */
/*  Linrad header: explicit byte-level I/O (no struct padding risk)   */
/* ------------------------------------------------------------------ */
typedef struct {
    int32_t  sentinel;
    double   timestamp;     /* seconds of day (WavViewDX convention) */
    double   passband_center;
    int32_t  passband_direction;
    int32_t  rx_input_mode;
    int32_t  rx_rf_channels;
    int32_t  rx_ad_channels;
    int32_t  rx_ad_speed;
    uint8_t  save_init_flag;
} LinradHdr;

static void linrad_from_bytes(LinradHdr *h, const uint8_t *b)
{
    uint64_t u64;
    h->sentinel = (int32_t)((uint32_t)b[0]|(uint32_t)b[1]<<8|
                             (uint32_t)b[2]<<16|(uint32_t)b[3]<<24);
    u64=(uint64_t)b[4]|(uint64_t)b[5]<<8|(uint64_t)b[6]<<16|
        (uint64_t)b[7]<<24|(uint64_t)b[8]<<32|(uint64_t)b[9]<<40|
        (uint64_t)b[10]<<48|(uint64_t)b[11]<<56;
    memcpy(&h->timestamp,&u64,8);
    u64=(uint64_t)b[12]|(uint64_t)b[13]<<8|(uint64_t)b[14]<<16|
        (uint64_t)b[15]<<24|(uint64_t)b[16]<<32|(uint64_t)b[17]<<40|
        (uint64_t)b[18]<<48|(uint64_t)b[19]<<56;
    memcpy(&h->passband_center,&u64,8);
    h->passband_direction=(int32_t)((uint32_t)b[20]|(uint32_t)b[21]<<8|
                                    (uint32_t)b[22]<<16|(uint32_t)b[23]<<24);
    h->rx_input_mode=(int32_t)((uint32_t)b[24]|(uint32_t)b[25]<<8|
                                (uint32_t)b[26]<<16|(uint32_t)b[27]<<24);
    h->rx_rf_channels=(int32_t)((uint32_t)b[28]|(uint32_t)b[29]<<8|
                                 (uint32_t)b[30]<<16|(uint32_t)b[31]<<24);
    h->rx_ad_channels=(int32_t)((uint32_t)b[32]|(uint32_t)b[33]<<8|
                                 (uint32_t)b[34]<<16|(uint32_t)b[35]<<24);
    h->rx_ad_speed=(int32_t)((uint32_t)b[36]|(uint32_t)b[37]<<8|
                              (uint32_t)b[38]<<16|(uint32_t)b[39]<<24);
    h->save_init_flag=b[40];
}

static void linrad_to_bytes(const LinradHdr *h, uint8_t *b)
{
    uint32_t u32; uint64_t u64;
    u32=(uint32_t)h->sentinel;
    b[0]=u32&0xFF;b[1]=(u32>>8)&0xFF;b[2]=(u32>>16)&0xFF;b[3]=(u32>>24)&0xFF;
    memcpy(&u64,&h->timestamp,8);
    b[4]=u64&0xFF;b[5]=(u64>>8)&0xFF;b[6]=(u64>>16)&0xFF;b[7]=(u64>>24)&0xFF;
    b[8]=(u64>>32)&0xFF;b[9]=(u64>>40)&0xFF;b[10]=(u64>>48)&0xFF;b[11]=(u64>>56)&0xFF;
    memcpy(&u64,&h->passband_center,8);
    b[12]=u64&0xFF;b[13]=(u64>>8)&0xFF;b[14]=(u64>>16)&0xFF;b[15]=(u64>>24)&0xFF;
    b[16]=(u64>>32)&0xFF;b[17]=(u64>>40)&0xFF;b[18]=(u64>>48)&0xFF;b[19]=(u64>>56)&0xFF;
    u32=(uint32_t)h->passband_direction;
    b[20]=u32&0xFF;b[21]=(u32>>8)&0xFF;b[22]=(u32>>16)&0xFF;b[23]=(u32>>24)&0xFF;
    u32=(uint32_t)h->rx_input_mode;
    b[24]=u32&0xFF;b[25]=(u32>>8)&0xFF;b[26]=(u32>>16)&0xFF;b[27]=(u32>>24)&0xFF;
    u32=(uint32_t)h->rx_rf_channels;
    b[28]=u32&0xFF;b[29]=(u32>>8)&0xFF;b[30]=(u32>>16)&0xFF;b[31]=(u32>>24)&0xFF;
    u32=(uint32_t)h->rx_ad_channels;
    b[32]=u32&0xFF;b[33]=(u32>>8)&0xFF;b[34]=(u32>>16)&0xFF;b[35]=(u32>>24)&0xFF;
    u32=(uint32_t)h->rx_ad_speed;
    b[36]=u32&0xFF;b[37]=(u32>>8)&0xFF;b[38]=(u32>>16)&0xFF;b[39]=(u32>>24)&0xFF;
    b[40]=h->save_init_flag;
}

/* ------------------------------------------------------------------ */
/*  Date / time helpers                                                */
/* ------------------------------------------------------------------ */
#define IS_LEAP(y) (((y)%4==0&&(y)%100!=0)||(y)%400==0)

static int64_t utc_to_unix(int year,int month,int day,
                             int hour,int minute,int second)
{
    static const int md[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    int y0=year-1,y1=1969;
    int leaps=(y0/4-y1/4)-(y0/100-y1/100)+(y0/400-y1/400);
    int64_t days=(int64_t)(year-1970)*365+leaps;
    int leap=IS_LEAP(year);
    for(int m=1;m<month;m++) days+=md[m]+(m==2?leap:0);
    days+=day-1;
    return days*86400LL+(int64_t)hour*3600LL+(int64_t)minute*60LL+second;
}

static void unix_to_utc(int64_t epoch,
                         int *year,int *month,int *day,
                         int *hour,int *minute,int *second)
{
    /* Simple iterative decomposition */
    int64_t rem=epoch;
    *second=(int)(rem%60); rem/=60;
    *minute=(int)(rem%60); rem/=60;
    *hour  =(int)(rem%24); rem/=24;
    /* days since 1970-01-01 */
    int y=1970;
    while(1){
        int dy=IS_LEAP(y)?366:365;
        if(rem<dy) break;
        rem-=dy; y++;
    }
    *year=y;
    static const int md[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    int leap=IS_LEAP(y);
    int m=1;
    while(m<=12){
        int dim=md[m]+(m==2?leap:0);
        if(rem<dim) break;
        rem-=dim; m++;
    }
    *month=m;
    *day=(int)rem+1;
}

static void next_day(int *y,int *m,int *d)
{
    static const int md[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    int leap=IS_LEAP(*y);
    int dim=md[*m]+(*m==2?leap:0);
    if(++(*d)>dim){*d=1;if(++(*m)>12){*m=1;++(*y);}}
}

static int day_of_week(int y,int m,int d)
{
    static const int t[]={0,3,2,5,0,3,5,1,4,6,2,4};
    if(m<3)y--;
    return(y+y/4-y/100+y/400+t[m-1]+d)%7;
}

/* ------------------------------------------------------------------ */
/*  String / basename helpers                                          */
/* ------------------------------------------------------------------ */
static const char *file_basename(const char *path)
{
    const char *b=strrchr(path,'/');
    if(!b) b=strrchr(path,'\\');
    return b ? b+1 : path;
}

static void strip_extension(char *stem, const char *path)
{
    const char *base=file_basename(path);
    size_t n=strlen(base); if(n>=512) n=511;
    memcpy(stem,base,n); stem[n]='\0';
    char *dot=strrchr(stem,'.'); if(dot) *dot='\0';
}

/* ------------------------------------------------------------------ */
/*  Input format detection and header reading                          */
/* ------------------------------------------------------------------ */

/* Parse WavViewDX filename: iq_pcm16_ch{N}_cf{hz}_sr{hz}_dt{yyyymmdd-hhmmss}
   Returns 1 on success, 0 on failure. */
/* Parse SDRuno filename: SDRuno_yyyymmdd_hhmmss_xxxxkHz_chN.wav
   Returns 1 on success, 0 if pattern not matched. */
static int parse_sdruno_filename(const char *path, RecInfo *r)
{
    char stem[512];
    strip_extension(stem, path);
    /* Find the basename */
    const char *bn = strrchr(stem, '\\');
    if(!bn) bn = strrchr(stem, '/');
    if(bn) bn++; else bn = stem;
    int yy=0,mo=0,dd=0,hh=0,mi=0,ss=0,cf_khz=0;
    if(sscanf(bn,"SDRuno_%4d%2d%2d_%2d%2d%2dZ_%dkHz",
              &yy,&mo,&dd,&hh,&mi,&ss,&cf_khz)>=7 ||
       sscanf(bn,"SDRuno_%4d%2d%2d_%2d%2d%2d_%dkHz",
              &yy,&mo,&dd,&hh,&mi,&ss,&cf_khz)>=7){
        r->year=yy; r->month=mo; r->day=dd;
        r->hour=hh; r->minute=mi; r->second=ss;
        r->centre_freq_hz=cf_khz*1000;
        r->epoch=utc_to_unix(yy,mo,dd,hh,mi,ss);
        return 1;
    }
    return 0;
}

static int parse_wavviewdx_filename(const char *path, RecInfo *r)
{
    char stem[512];
    strip_extension(stem, path);

    /* Locate the iq_pcm16_ch pattern anywhere in the stem.
     * This allows filenames with leading prefixes (e.g. IN_iq_pcm16_...) */
    char *pat=strstr(stem,"iq_pcm16_ch");
    if(!pat) return 0;

    /* ch field */
    int ch=0;
    if(sscanf(pat+11,"%d",&ch)!=1) return 0;
    r->num_channels = ch * 2;   /* ch1=2 samples, ch2=4 samples */
    r->tuner = (ch==2) ? TUNER_DUAL : TUNER_SINGLE;

    /* cf field */
    char *cf_ptr=strstr(pat,"_cf");
    if(!cf_ptr) return 0;
    if(sscanf(cf_ptr+3,"%d",&r->centre_freq_hz)!=1) return 0;

    /* sr field */
    char *sr_ptr=strstr(pat,"_sr");
    if(!sr_ptr) return 0;
    if(sscanf(sr_ptr+3,"%d",&r->sample_rate)!=1) return 0;

    /* dt field */
    char *dt_ptr=strstr(pat,"_dt");
    if(!dt_ptr) return 0;
    int yy,mo,dd,hh,mi,ss;
    if(sscanf(dt_ptr+3,"%4d%2d%2d-%2d%2d%2d",
              &yy,&mo,&dd,&hh,&mi,&ss)!=6) return 0;
    r->year=yy; r->month=mo; r->day=dd;
    r->hour=hh; r->minute=mi; r->second=ss;
    r->epoch = utc_to_unix(yy,mo,dd,hh,mi,ss);
    r->utc_offset = 0;
    r->fmt = FMT_WAVVIEWDX;
    r->data_offset = 0;
    return 1;
}

/* Parse Linrad filename: yyyymmdd_hhmmssU_xxxxkHz[.raw]
   Also fills utc_offset. Returns 1 on success. */
static int parse_linrad_filename(const char *path, RecInfo *r,
                                  char *freq_str_out,  /* >=16 bytes */
                                  char *suffix_out)    /* >=64 bytes */
{
    char stem[512];
    strip_extension(stem, path);
    if(strlen(stem)<20) return 0;

    char tmp[9];
    memcpy(tmp,stem,8); tmp[8]='\0';
    if(sscanf(tmp,"%4d%2d%2d",&r->year,&r->month,&r->day)!=3) return 0;
    if(stem[8]!='_') return 0;
    memcpy(tmp,stem+9,6); tmp[6]='\0';
    if(sscanf(tmp,"%2d%2d%2d",&r->hour,&r->minute,&r->second)!=3) return 0;

    char uc=stem[15];
    r->utc_offset=(uc>='0'&&uc<='9')?(uc-'0'):0;

    if(stem[16]!='_') return 0;
    { size_t n=strlen(stem+16); if(n>63)n=63;
      memcpy(suffix_out,stem+16,n); suffix_out[n]='\0'; }

    const char *fs=stem+17;
    char *khz=strstr(fs,"kHz");
    if(!khz) return 0;
    size_t fl=(size_t)(khz-fs); if(fl>=16)fl=15;
    memcpy(freq_str_out,fs,fl); freq_str_out[fl]='\0';
    r->centre_freq_hz = atoi(freq_str_out)*1000;
    r->epoch = utc_to_unix(r->year,r->month,r->day,r->hour,r->minute,r->second);
    return 1;
}

/* Detect input format and fill RecInfo. Returns 1 on success. */
static int detect_input(const char *path, RecInfo *r, FILE *fp,
                         LinradHdr *linrad_hdr_out)
{
    memset(r,0,sizeof(*r));
    { size_t n=strlen(path); if(n>=512)n=511;
      memcpy(r->filepath,path,n); r->filepath[n]='\0'; }

    uint8_t magic[12];
    rewind(fp);
    size_t got=fread(magic,1,12,fp);
    if(got<4){ proc_log("Error: file too small.\n"); return 0; }

    /* ── Linrad raw ── */
    if(magic[0]==0xFF && magic[1]==0xFF && magic[2]==0xFF && magic[3]==0xFF){
        uint8_t hbuf[LINRAD_HDR_SIZE];
        rewind(fp);
        if(fread(hbuf,1,LINRAD_HDR_SIZE,fp)!=LINRAD_HDR_SIZE){
            proc_log("Error: cannot read Linrad header.\n"); return 0; }
        linrad_from_bytes(linrad_hdr_out,hbuf);
        if(linrad_hdr_out->sentinel!=-1){
            proc_log("Error: invalid Linrad sentinel.\n"); return 0; }
        r->fmt        = FMT_LINRAD;
        r->sample_rate= linrad_hdr_out->rx_ad_speed;
        r->centre_freq_hz=(int)(linrad_hdr_out->passband_center*1e6+0.5);
        r->num_channels= linrad_hdr_out->rx_ad_channels;
        r->tuner = (r->num_channels==4) ? TUNER_DUAL : TUNER_SINGLE;
        r->epoch  = (int64_t)linrad_hdr_out->timestamp;
        unix_to_utc(r->epoch,&r->year,&r->month,&r->day,
                              &r->hour,&r->minute,&r->second);
        /* Try to get utc_offset from filename */
        char fs[16], suf[64];
        parse_linrad_filename(path,r,fs,suf);  /* fills utc_offset */
        r->data_offset = LINRAD_HDR_SIZE;
        file_seek(fp,0,SEEK_END);
        int64_t fsz=file_tell(fp);
        r->data_bytes=(uint64_t)(fsz-(int64_t)LINRAD_HDR_SIZE);
        return 1;
    }

    /* ── WAV family ── */
    if(magic[0]=='R'&&magic[1]=='I'&&magic[2]=='F'&&magic[3]=='F'){
        /* Confirm WAVE */
        if(got<12||magic[8]!='W'||magic[9]!='A'||magic[10]!='V'||magic[11]!='E'){
            proc_log("Error: RIFF file is not WAVE format.\n"); return 0; }

        /* Read chunk at offset 12 to distinguish SDRuno vs SDR Connect */
        uint8_t chunk_id[4];
        file_seek(fp,12,SEEK_SET);
        if(fread(chunk_id,1,4,fp)!=4){
            proc_log("Error: cannot read WAV chunk.\n"); return 0; }

        /* SDR Connect: first chunk is JUNK */
        if(chunk_id[0]=='J'&&chunk_id[1]=='U'&&chunk_id[2]=='N'&&chunk_id[3]=='K'){
            r->fmt = FMT_SDRCONNECT;
            /* JUNK size */
            uint32_t junk_sz;
            if(fread(&junk_sz,4,1,fp)!=1){ return 0; }
            /* fmt chunk follows JUNK */
            int64_t fmt_off=12+8+(int64_t)junk_sz;
            file_seek(fp,fmt_off,SEEK_SET);
            uint8_t fmt_id[4];
            if(fread(fmt_id,1,4,fp)!=4)return 0;
            if(fmt_id[0]!='f'||fmt_id[1]!='m'||fmt_id[2]!='t'||fmt_id[3]!=' '){
                proc_log("Error: expected fmt chunk in SDR Connect file.\n");
                return 0; }
            uint32_t fmt_sz;
            if(fread(&fmt_sz,4,1,fp)!=1)return 0;
            FmtChunk fmt;
            if(fread(&fmt,sizeof(fmt),1,fp)!=1)return 0;
            r->sample_rate      = (int)fmt.sample_rate;
            r->num_channels     = (int)fmt.num_channels;
            r->bits_per_sample  = (int)fmt.bits_per_sample;
            r->tuner            = (r->num_channels==4)?TUNER_DUAL:TUNER_SINGLE;
            /* data chunk */
            int64_t data_off=fmt_off+8+(int64_t)fmt_sz;
            file_seek(fp,data_off,SEEK_SET);
            uint8_t data_id[4];
            if(fread(data_id,1,4,fp)!=4)return 0;
            uint32_t data_sz;
            if(fread(&data_sz,4,1,fp)!=1)return 0;
            r->data_offset=(int64_t)(data_off+8);
            r->data_bytes=(uint64_t)data_sz;
            /* metadata from filename: SDRconnect_IQ_yyyymmdd_hhmmss_ffffffHZ */
            {
                char stem[512]; strip_extension(stem,path);
                int yy,mo,dd,hh,mi,ss,ff=0;
                if(sscanf(stem,"SDRconnect_IQ_%4d%2d%2d_%2d%2d%2d_%dHZ",
                          &yy,&mo,&dd,&hh,&mi,&ss,&ff)==7){
                    r->year=yy;r->month=mo;r->day=dd;
                    r->hour=hh;r->minute=mi;r->second=ss;
                    r->centre_freq_hz=ff;
                    r->epoch=utc_to_unix(yy,mo,dd,hh,mi,ss);
                } else {
                    proc_log(
                        "Warning: cannot parse SDR Connect filename for metadata.\n"
                        "  Expected: SDRconnect_IQ_yyyymmdd_hhmmss_ffffffHZ\n");
                    r->centre_freq_hz=0;
                    r->epoch=(int64_t)time(NULL);
                    unix_to_utc(r->epoch,&r->year,&r->month,&r->day,
                                          &r->hour,&r->minute,&r->second);
                }
            }
            return 1;
        }

        /* SDRuno: fmt chunk at offset 12 */
        if(chunk_id[0]=='f'&&chunk_id[1]=='m'&&chunk_id[2]=='t'&&chunk_id[3]==' '){
            r->fmt=FMT_SDRUNO;
            uint32_t fmt_sz;
            if(fread(&fmt_sz,4,1,fp)!=1)return 0;
            FmtChunk fmt;
            if(fread(&fmt,sizeof(fmt),1,fp)!=1)return 0;
            r->sample_rate      = (int)fmt.sample_rate;
            r->num_channels     = (int)fmt.num_channels;
            r->bits_per_sample  = (int)fmt.bits_per_sample;
            r->tuner            = (r->num_channels==4)?TUNER_DUAL:TUNER_SINGLE;
            /* Skip any fmt extension bytes */
            if(fmt_sz>sizeof(fmt))
                file_seek(fp,(int64_t)(fmt_sz-sizeof(fmt)),SEEK_CUR);
            else if(fmt_sz<sizeof(fmt)){
                /* Undersized fmt — seek to end of chunk */
                file_seek(fp,12+8+(int64_t)fmt_sz,SEEK_SET);
            }
            /* Walk remaining chunks to find auxi and data */
            while(1){
                uint8_t cid[4]; if(fread(cid,1,4,fp)!=4) break;
                uint32_t csz;   if(fread(&csz,4,1,fp)!=1) break;
                if(cid[0]=='a'&&cid[1]=='u'&&cid[2]=='x'&&cid[3]=='i'){
                    if(csz>=sizeof(AuxiChunk)){
                        AuxiChunk auxi;
                        if(fread(&auxi,sizeof(auxi),1,fp)==1){
                            r->year  =auxi.start_year;  r->month =auxi.start_month;
                            r->day   =auxi.start_day;   r->hour  =auxi.start_hour;
                            r->minute=auxi.start_minute;r->second=auxi.start_second;
                            r->centre_freq_hz=auxi.centre_freq_hz;
                            r->epoch=utc_to_unix(r->year,r->month,r->day,
                                                  r->hour,r->minute,r->second);
                        }
                        if(csz>sizeof(AuxiChunk))
                            file_seek(fp,(int64_t)(csz-sizeof(AuxiChunk)),SEEK_CUR);
                    } else {
                        file_seek(fp,(int64_t)csz,SEEK_CUR);
                    }
                } else if(cid[0]=='r'&&cid[1]=='c'&&cid[2]=='v'&&cid[3]=='r'){
                    /* Perseus (flags=4) or Jaguar (flags=5) */
                    if(csz>=sizeof(RcvrChunk)){
                        RcvrChunk rcvr;
                        if(fread(&rcvr,sizeof(rcvr),1,fp)==1){
                            r->rcvr = rcvr;
                            r->centre_freq_hz = (int)rcvr.centre_freq_hz;
                            r->epoch          = (int64_t)rcvr.unix_timestamp;
                            unix_to_utc(r->epoch,&r->year,&r->month,&r->day,
                                                  &r->hour,&r->minute,&r->second);
                            /* flags=5 -> Jaguar 1.6MHz; flags=4 -> check padding byte 16:
                             * Perseus has padding[4]=0x01, Jaguar 2MHz has padding[4]=0x00 */
                            if(rcvr.flags==5)
                                r->fmt = FMT_JAGUAR;
                            else if(rcvr.flags==4 && rcvr.padding[4]==0x00)
                                r->fmt = FMT_JAGUAR;
                            else
                                r->fmt = FMT_PERSEUS;
                        }
                        if(csz>sizeof(RcvrChunk))
                            file_seek(fp,(int64_t)(csz-sizeof(RcvrChunk)),SEEK_CUR);
                    } else {
                        file_seek(fp,(int64_t)csz,SEEK_CUR);
                    }
                } else if(cid[0]=='d'&&cid[1]=='a'&&cid[2]=='t'&&cid[3]=='a'){
                    r->data_offset=file_tell(fp);
                    r->data_bytes=(uint64_t)csz;
                    /* If RIFF size was 0xFFFFFFFF placeholder, use file size */
                    if(r->data_bytes==0xFFFFFFFF){
                        file_seek(fp,0,SEEK_END);
                        int64_t fsz=file_tell(fp);
                        r->data_bytes=(uint64_t)(fsz-r->data_offset);
                    }
                    break;
                } else {
                    file_seek(fp,(int64_t)csz+(csz&1),SEEK_CUR);
                }
            }
            /* Fallback: if auxi gave no date, try filename */
            if(r->year==0){
                RecInfo tmp; memset(&tmp,0,sizeof(tmp));
                if(parse_sdruno_filename(path,&tmp)){
                    r->year=tmp.year; r->month=tmp.month; r->day=tmp.day;
                    r->hour=tmp.hour; r->minute=tmp.minute; r->second=tmp.second;
                    if(r->centre_freq_hz==0) r->centre_freq_hz=tmp.centre_freq_hz;
                    r->epoch=utc_to_unix(r->year,r->month,r->day,
                                          r->hour,r->minute,r->second);
                }
            }
            return 1;
        }

        proc_log("Error: unrecognised WAV chunk layout.\n");
        return 0;
    }

    /* ── RF64 WAV ── */
    if(magic[0]=='R'&&magic[1]=='F'&&magic[2]=='6'&&magic[3]=='4'){
        /* Handle RF64 similarly to RIFF  -  scan for fmt and data chunks */
        r->fmt=FMT_SDRUNO;   /* RF64 WAV is SDRuno format */
        file_seek(fp,12,SEEK_SET);
        /* skip ds64 */
        uint8_t cid[4];
        uint32_t csz;
        if(fread(cid,1,4,fp)!=4||fread(&csz,4,1,fp)!=1) return 0;
        file_seek(fp,(int64_t)csz,SEEK_CUR);
        /* now scan for fmt and auxi */
        while(1){
            if(fread(cid,1,4,fp)!=4) break;
            if(fread(&csz,4,1,fp)!=1) break;
            if(cid[0]=='f'&&cid[1]=='m'&&cid[2]=='t'&&cid[3]==' '){
                FmtChunk fmt;
                if(fread(&fmt,sizeof(fmt),1,fp)!=1) break;
                r->sample_rate     =(int)fmt.sample_rate;
                r->num_channels    =(int)fmt.num_channels;
                r->bits_per_sample =(int)fmt.bits_per_sample;
                r->tuner=(r->num_channels==4)?TUNER_DUAL:TUNER_SINGLE;
                if(csz>sizeof(fmt)) file_seek(fp,(int64_t)(csz-sizeof(fmt)),SEEK_CUR);
            } else if(cid[0]=='a'&&cid[1]=='u'&&cid[2]=='x'&&cid[3]=='i'){
                AuxiChunk auxi;
                if(fread(&auxi,sizeof(auxi),1,fp)!=1) break;
                r->year=auxi.start_year; r->month=auxi.start_month;
                r->day=auxi.start_day;   r->hour=auxi.start_hour;
                r->minute=auxi.start_minute; r->second=auxi.start_second;
                r->centre_freq_hz=auxi.centre_freq_hz;
                r->epoch=utc_to_unix(r->year,r->month,r->day,
                                      r->hour,r->minute,r->second);
                if(csz>sizeof(auxi)) file_seek(fp,(int64_t)(csz-sizeof(auxi)),SEEK_CUR);
            } else if(cid[0]=='d'&&cid[1]=='a'&&cid[2]=='t'&&cid[3]=='a'){
                r->data_offset=file_tell(fp);
                /* true size from ds64  -  for now use file size */
                file_seek(fp,0,SEEK_END);
                int64_t fsz=file_tell(fp);
                r->data_bytes=(uint64_t)(fsz-r->data_offset);
                return 1;
            } else {
                file_seek(fp,(int64_t)csz+(csz&1),SEEK_CUR);
            }
        }
        /* Fallback: if auxi gave no date, try filename.
           Also handles RF64 SDR Connect (JUNK chunk present, no auxi). */
        if(r->year==0){
            /* Try SDRuno filename first */
            RecInfo tmp; memset(&tmp,0,sizeof(tmp));
            if(parse_sdruno_filename(path,&tmp)){
                r->year=tmp.year; r->month=tmp.month; r->day=tmp.day;
                r->hour=tmp.hour; r->minute=tmp.minute; r->second=tmp.second;
                if(r->centre_freq_hz==0) r->centre_freq_hz=tmp.centre_freq_hz;
                r->epoch=utc_to_unix(r->year,r->month,r->day,
                                      r->hour,r->minute,r->second);
            }
            /* Try SDR Connect filename */
            if(r->year==0){
                char stem[512]; strip_extension(stem,path);
                const char *bn=strrchr(stem,'\\');
                if(!bn) bn=strrchr(stem,'/');
                if(bn) bn++; else bn=stem;
                int yy=0,mo=0,dd=0,hh=0,mi=0,ss=0,ff=0;
                if(sscanf(bn,"SDRconnect_IQ_%4d%2d%2d_%2d%2d%2d_%dHZ",
                          &yy,&mo,&dd,&hh,&mi,&ss,&ff)==7){
                    r->fmt=FMT_SDRCONNECT;
                    r->year=yy;r->month=mo;r->day=dd;
                    r->hour=hh;r->minute=mi;r->second=ss;
                    r->centre_freq_hz=ff;
                    r->epoch=utc_to_unix(yy,mo,dd,hh,mi,ss);
                }
            }
        }
        return 1;
    }

    /* ── WavViewDX raw (headerless) ── */
    if(parse_wavviewdx_filename(path,r)){
        r->data_offset=0;
        file_seek(fp,0,SEEK_END);
        r->data_bytes=(uint64_t)file_tell(fp);
        return 1;
    }

    proc_log(
        "Error: cannot detect input format for '%s'.\n"
        "  Supported formats: Linrad raw, WavViewDX raw, SDRuno WAV, SDR Connect WAV\n",
        path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Output filename builders                                           */
/* ------------------------------------------------------------------ */

static void build_linrad_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int utc_offset,int freq_hz,int out_channels)
{
    int kHz=freq_hz/1000;
    (void)out_channels;
    /* Use 'Z' for UTC (offset=0), digit for other offsets */
    char utc_ch = (utc_offset==0) ? 'Z' : ('0'+utc_offset);
    snprintf(out,sz,"%04d%02d%02d_%02d%02d%02d%c_%dkHz.raw",
             year,month,day,hh,mm,ss,utc_ch,kHz);
}

static void build_wavviewdx_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int freq_hz,int sample_rate,int out_channels)
{
    /* ch field: 1=single IQ stream, 2=dual IQ streams (4 int16/frame)
     * For ch1/ch2 extraction from dual, output is single IQ so ch=1.
     * We use ch=1 always for extracted channels since output is always 2 int16/frame. */
    int ch = (out_channels==4) ? 2 : 1;
    snprintf(out,sz,"iq_pcm16_ch%d_cf%d_sr%d_dt%04d%02d%02d-%02d%02d%02d",
             ch,freq_hz,sample_rate,year,month,day,hh,mm,ss);
}

static void build_sdruno_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int freq_hz)
{
    int kHz=freq_hz/1000;
    snprintf(out,sz,"SDRuno_%04d%02d%02d_%02d%02d%02dZ_%dkHz.wav",
             year,month,day,hh,mm,ss,kHz);
}

static void build_sdrconnect_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int freq_hz)
{
    snprintf(out,sz,"SDRconnect_IQ_%04d%02d%02d_%02d%02d%02d_%dHZ.wav",
             year,month,day,hh,mm,ss,freq_hz);
}

static void build_rcvr_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int freq_hz, FileFormat fmt)
{
    /* Use a timestamp-based filename matching the style of the other formats */
    const char *prefix = (fmt==FMT_JAGUAR) ? "Jaguar" : "Perseus";
    snprintf(out,sz,"%s_%04d%02d%02d_%02d%02d%02dZ_%dkHz.wav",
             prefix,year,month,day,hh,mm,ss,freq_hz/1000);
}

/* ------------------------------------------------------------------ */
/*  WAV output helpers: RIFF/RF64 write + patch                       */
/* ------------------------------------------------------------------ */

static int64_t write_rf64_ds64(FILE *fp)
{
    fwrite("RF64",1,4,fp);
    uint32_t neg1=0xFFFFFFFF; fwrite(&neg1,4,1,fp);
    fwrite("WAVE",1,4,fp);
    fwrite("ds64",1,4,fp);
    uint32_t ds64sz=(uint32_t)sizeof(Ds64Payload); fwrite(&ds64sz,4,1,fp);
    int64_t pos=file_tell(fp);
    Ds64Payload ds64; memset(&ds64,0,sizeof(ds64)); fwrite(&ds64,sizeof(ds64),1,fp);
    return pos;
}

static void patch_rf64(FILE *fp,int64_t ds64_pos,int64_t data_sz32_pos,
                        uint64_t file_size,uint64_t data_size,uint64_t frames)
{
    file_seek(fp,ds64_pos,SEEK_SET);
    Ds64Payload ds64;
    ds64.riff_size=file_size-8; ds64.data_size=data_size;
    ds64.sample_count=frames; ds64.table_len=0;
    fwrite(&ds64,sizeof(ds64),1,fp);
    file_seek(fp,data_sz32_pos,SEEK_SET);
    uint32_t neg1=0xFFFFFFFF; fwrite(&neg1,4,1,fp);
}

static void write_fmt_pcm(FILE *fp,uint32_t sr,uint16_t ch,uint16_t bps)
{
    fwrite("fmt ",1,4,fp);
    uint32_t sz=16; fwrite(&sz,4,1,fp);
    FmtChunk fmt;
    fmt.audio_format=0x0001; fmt.num_channels=ch;
    fmt.sample_rate=sr; fmt.bits_per_sample=bps;
    fmt.block_align=(uint16_t)(ch*(bps/8));
    fmt.byte_rate=sr*fmt.block_align;
    fwrite(&fmt,sizeof(fmt),1,fp);
}

/* Write SDRuno auxi chunk */
static void write_auxi(FILE *fp,int32_t cf_hz,
    int sy,int sm,int sd,int sh,int smin,int ssec,
    int ey,int em,int ed,int eh,int emin,int esec,
    const char *fname)
{
    fwrite("auxi",1,4,fp);
    uint32_t sz=(uint32_t)sizeof(AuxiChunk); fwrite(&sz,4,1,fp);
    AuxiChunk a; memset(&a,0,sizeof(a));
    a.start_year=(uint16_t)sy; a.start_month=(uint16_t)sm;
    a.start_day=(uint16_t)sd;  a.start_dow=(uint16_t)day_of_week(sy,sm,sd);
    a.start_hour=(uint16_t)sh; a.start_minute=(uint16_t)smin;
    a.start_second=(uint16_t)ssec;
    a.stop_year=(uint16_t)ey;  a.stop_month=(uint16_t)em;
    a.stop_day=(uint16_t)ed;   a.stop_dow=(uint16_t)day_of_week(ey,em,ed);
    a.stop_hour=(uint16_t)eh;  a.stop_minute=(uint16_t)emin;
    a.stop_second=(uint16_t)esec;
    a.centre_freq_hz=cf_hz;
    { size_t n=strlen(fname); if(n>83)n=83;
      memcpy(a.filename,fname,n); a.filename[n]='\0'; }
    fwrite(&a,sizeof(a),1,fp);
}

/* Write SDR Connect JUNK chunk (28 zero bytes) */
static void write_junk(FILE *fp)
{
    fwrite("JUNK",1,4,fp);
    uint32_t sz=28; fwrite(&sz,4,1,fp);
    uint8_t zeroes[28]; memset(zeroes,0,28);
    fwrite(zeroes,28,1,fp);
}

/*
 * Write the complete output WAV header and return the file position
 * of the data chunk size field (for patching later).
 * For RIFF: ds64_pos_out = -1.
 */
static int64_t write_wav_header(FILE *fp,
    FileFormat out_fmt,
    uint64_t out_data_bytes,
    uint32_t sample_rate, uint16_t channels,
    uint16_t bits_per_sample,
    int32_t cf_hz,
    int sy,int sm,int sd,int sh,int smin,int ssec,
    int ey,int em,int ed,int eh,int emin,int esec,
    const char *outpath,
    int *use_rf64_out, int64_t *ds64_pos_out)
{
    /* Compute total header overhead:
       SDRuno:       RIFF(12) + fmt(8+16) + auxi(8+164) + data(8) = 216
       SDR Connect:  RIFF(12) + JUNK(8+28) + fmt(8+16) + data(8) = 80
       Perseus/Jag:  RIFF(12) + fmt(8+16) + rcvr(8+34) + data(8) = 86
    */
    uint64_t overhead = (out_fmt==FMT_SDRUNO)    ? 216 :
                        (out_fmt==FMT_SDRCONNECT) ?  80 :
                        (out_fmt==FMT_PERSEUS||out_fmt==FMT_JAGUAR) ? 86 : 80;
    /* SDRuno does not support RF64. For SDRuno format, use RIFF with
     * 0xFFFFFFFF clamped size (SDRuno's own behaviour for large files).
     * Only use RF64 for SDR Connect format if it exceeds 4GB. */
    int use_rf64 = (out_fmt==FMT_SDRCONNECT) &&
                   (out_data_bytes >= 0xFFFFFFFFULL - overhead);
    *use_rf64_out = use_rf64;
    *ds64_pos_out = -1;

    if(use_rf64){
        *ds64_pos_out = write_rf64_ds64(fp);
    } else {
        fwrite("RIFF",1,4,fp);
        /* Clamp to 0xFFFFFFFF for large files (matches SDRuno's own behaviour) */
        uint64_t riff_total = overhead - 8 + out_data_bytes;
        uint32_t riff_sz = (riff_total > 0xFFFFFFFFULL) ?
                           0xFFFFFFFFU : (uint32_t)riff_total;
        fwrite(&riff_sz,4,1,fp);
        fwrite("WAVE",1,4,fp);
    }

    if(out_fmt==FMT_SDRCONNECT){
        write_junk(fp);
    }

    write_fmt_pcm(fp,sample_rate,channels,bits_per_sample);

    if(out_fmt==FMT_SDRUNO){
        write_auxi(fp,cf_hz,sy,sm,sd,sh,smin,ssec,ey,em,ed,eh,emin,esec,outpath);
    }
    if(out_fmt==FMT_PERSEUS||out_fmt==FMT_JAGUAR){
        /* Build rcvr chunk. flags field encodes the sample rate index:
         *   0=125k, 1=250k, 2=500k, 3=1000k, 4=2000k (16-bit), 5=Jaguar
         * Constant padding bytes differ between Perseus and Jaguar — set below. */
        RcvrChunk rc; memset(&rc,0,sizeof(rc));
        rc.centre_freq_hz  = (uint32_t)cf_hz;
        if(out_fmt==FMT_JAGUAR){
            /* flags=5 for native Jaguar 1,600,000 Hz;
             * flags=4 for 2,000,000 Hz (WavViewDX uses fmt chunk rate for this index) */
            rc.flags = (sample_rate==2000000) ? 4 : 5;
        } else {
            /* Map sample rate to Perseus rate index */
            rc.flags = (sample_rate<=125000)?0:
                       (sample_rate<=250000)?1:
                       (sample_rate<=500000)?2:
                       (sample_rate<=1000000)?3:4;
        }
        rc.unix_timestamp  = (uint32_t)utc_to_unix(sy,sm,sd,sh,smin,ssec);
        /* Constant padding bytes verified from genuine hardware recordings:
         * Perseus (all rates): padding[2,4,5] = 0x01  (rcvr bytes 14,16,17)
         * Jaguar 1.6 MHz:      padding[2,3,5] = 0x01  (rcvr bytes 14,15,17)
         * Jaguar 2.0 MHz:      padding[2,5]   = 0x01  (rcvr bytes 14,17) */
        if(out_fmt==FMT_JAGUAR && sample_rate==1600000){
            rc.padding[2] = 0x01;                          /* rcvr+14 */
            rc.padding[3] = 0x01;                          /* rcvr+15 */
            rc.padding[5] = 0x01;                          /* rcvr+17 */
        } else if(out_fmt==FMT_JAGUAR && sample_rate==2000000){
            rc.padding[2] = 0x01;                          /* rcvr+14 */
            rc.padding[5] = 0x01;                          /* rcvr+17 */
        } else {
            /* Perseus — all rates */
            rc.padding[2] = 0x01;                          /* rcvr+14 */
            rc.padding[4] = 0x01;                          /* rcvr+16 */
            rc.padding[5] = 0x01;                          /* rcvr+17 */
        }
        fwrite("rcvr",1,4,fp);
        uint32_t rcvr_sz=sizeof(RcvrChunk);
        fwrite(&rcvr_sz,4,1,fp);
        fwrite(&rc,sizeof(rc),1,fp);
    }

    fwrite("data",1,4,fp);
    int64_t data_sz32_pos=file_tell(fp);
    if(use_rf64){
        uint32_t neg1=0xFFFFFFFF; fwrite(&neg1,4,1,fp);
    } else {
        uint32_t dsz=(uint32_t)out_data_bytes; fwrite(&dsz,4,1,fp);
    }
    return data_sz32_pos;
}

static void finalise_wav(FILE *fp,int use_rf64,
                          int64_t ds64_pos,int64_t data_sz32_pos,
                          int out_ch_count, int out_bps)
{
    int64_t total=file_tell(fp);
    uint64_t actual_data=(uint64_t)(total-(data_sz32_pos+4));
    uint64_t actual_frames=actual_data/((uint64_t)out_ch_count*(uint64_t)out_bps);

    if(use_rf64){
        patch_rf64(fp,ds64_pos,data_sz32_pos,
                   (uint64_t)total,actual_data,actual_frames);
    } else {
        /* Clamp to 0xFFFFFFFF for files >4GB (sentinel value, software uses file size) */
        file_seek(fp,4,SEEK_SET);
        uint32_t riff_sz = ((uint64_t)total-8 > 0xFFFFFFFFULL) ?
                            0xFFFFFFFFU : (uint32_t)((uint64_t)total-8);
        fwrite(&riff_sz,4,1,fp);
        file_seek(fp,data_sz32_pos,SEEK_SET);
        uint32_t data_sz = (actual_data > 0xFFFFFFFFULL) ?
                            0xFFFFFFFFU : (uint32_t)actual_data;
        fwrite(&data_sz,4,1,fp);
    }
}

/* ------------------------------------------------------------------ */
/*  Progress bar                                                       */
/* ------------------------------------------------------------------ */
static time_t _prog_start = 0;

static void progress_print(const char *label __attribute__((unused)),uint64_t done,uint64_t total)
{
    /* Post percent to GUI */
    int pct=(total>0)?(int)((double)done/(double)total*100.0):0;
    if(pct>100)pct=100;
    PostMessageW(g_proc_hwnd,WM_UPDATEPROG,(WPARAM)pct,0);
    /* ETA calculation */
    if(done==0) _prog_start=time(NULL);
    char eta[24]="ETA --:--:--";
    if(done>0&&done<total&&_prog_start>0){
        time_t el=time(NULL)-_prog_start;
        if(el>0){
            long rem=(long)(((double)(total-done)/(double)done)*(double)el+0.5);
            snprintf(eta,sizeof(eta),"ETA %02d:%02d:%02d",
                     (int)(rem/3600),(int)((rem%3600)/60),(int)(rem%60));
        }
    } else if(done>=total&&_prog_start>0){
        time_t el=time(NULL)-_prog_start;
        snprintf(eta,sizeof(eta),"Done %02d:%02d:%02d",
                 (int)(el/3600),(int)((el%3600)/60),(int)(el%60));
    }
    /* Post ETA string */
    wchar_t weta[32]; MultiByteToWideChar(CP_ACP,0,eta,-1,weta,32);
    PostMessageW(g_proc_hwnd,WM_UPDATEETA,0,(LPARAM)_wcsdup(weta));
}
static void progress_done(void){ PostMessageW(g_proc_hwnd,WM_UPDATEPROG,100,0); }

/* ------------------------------------------------------------------ */
/*  FIR filter (Kaiser-windowed sinc)                                  */
/* ------------------------------------------------------------------ */
static double bessel_i0(double x)
{
    double sum=1.0,term=1.0,xh=x/2.0;
    for(int k=1;k<=30;k++){
        term*=(xh/(double)k)*(xh/(double)k);
        sum+=term;
        if(term<1e-12*sum) break;
    }
    return sum;
}

static double *design_fir(double cutoff_norm,double trans_norm,
                            double atten_db,int *n_taps_out)
{
    double beta=(atten_db>=50.0)?0.1102*(atten_db-8.7):
                (atten_db>=21.0)?0.5842*pow(atten_db-21.0,0.4)+
                                 0.07886*(atten_db-21.0):0.0;
    int N=(int)ceil((atten_db-8.0)/(2.285*2.0*M_PI*trans_norm));
    if(N<3) N=3;
    if(N%2==0) N++;
    *n_taps_out=N;
    double *h=(double*)malloc((size_t)N*sizeof(double));
    if(!h) return NULL;
    double I0b=bessel_i0(beta);
    int M=(N-1)/2;
    for(int n=0;n<N;n++){
        double t=(double)(n-M);
        double sinc=(t==0.0)?2.0*cutoff_norm:sin(2.0*M_PI*cutoff_norm*t)/(M_PI*t);
        double r=(double)(n-M)/(double)M;
        double w=bessel_i0(beta*sqrt(fabs(1.0-r*r)))/I0b;
        h[n]=sinc*w;
    }
    double sum=0.0; for(int n=0;n<N;n++) sum+=h[n];
    for(int n=0;n<N;n++) h[n]/=sum;
    return h;
}

/* ------------------------------------------------------------------ */
/*  DDC — optimised polyphase decimation                               */
/*                                                                      */
/*  Improvements over original:                                         */
/*    • Polyphase: only 1 subfilter computed per output (1/D of work)   */
/*    • float arithmetic (2x throughput vs double for 16-bit input)     */
/*    • Circular delay line (no memmove per sample)                     */
/*    • Running phase rotation (no cos/sin per sample)                  */
/* ------------------------------------------------------------------ */
#define COPY_BUF_FRAMES 65536
#define CLIP16(x) ((int16_t)((x)>32767?32767:((x)<-32768?-32768:(int16_t)((x)+0.5))))

static volatile BOOL g_cancel;  /* forward declaration — defined in GUI globals */
static volatile BOOL g_pause;   /* forward declaration — defined in GUI globals */

/* Sequence file list — shared between GUI and processing engine */
static wchar_t   g_seq_paths[SEQ_MAX][MAX_PATH];
static int       g_seq_count = 0;

/* ------------------------------------------------------------------ */
/*  Sequential file reader — transparently spans multiple input files  */
/* ------------------------------------------------------------------ */
typedef struct {
    wchar_t  paths[SEQ_MAX][MAX_PATH];   /* wide paths */
    int      count;         /* total files */
    int      cur;           /* current file index */
    FILE    *fp;            /* current open file handle */
    int64_t  data_offset;   /* byte offset to data in current file */
    int      in_ch;         /* channels per frame */
    int      sample_rate;   /* samples per second */
    int      bytes_per_samp;/* 2=int16, 3=int24 */
} SeqReader;

/* Read exactly n_frames frames from the sequence, spanning files as needed.
 * For 24-bit input, converts each sample to 16-bit (top 16 of 24 bits).
 * Returns number of frames actually read (may be < n_frames at end of sequence). */
static size_t seq_read_frames(SeqReader *sr, int16_t *buf, size_t n_frames)
{
    int bps = (sr->bytes_per_samp > 0) ? sr->bytes_per_samp : 2;
    size_t total = 0;
    while(total < n_frames && sr->cur < sr->count){
        if(!sr->fp){
            sr->fp = _wfopen(sr->paths[sr->cur], L"rb");
            if(!sr->fp){ sr->cur++; continue; }
            file_seek(sr->fp, sr->data_offset, SEEK_SET);
        }
        size_t want = n_frames - total;
        int    spf  = sr->in_ch;   /* samples per frame */
        if(bps == 2){
            size_t got = fread(buf + total*(size_t)spf,
                               sizeof(int16_t), want*(size_t)spf, sr->fp);
            size_t gf  = got / (size_t)spf;
            total += gf;
            if(gf < want){ fclose(sr->fp); sr->fp=NULL; sr->cur++; }
        } else {
            /* 24-bit: read raw bytes then convert to int16 */
            size_t  raw  = want*(size_t)spf*3;
            uint8_t *tmp = (uint8_t*)malloc(raw);
            if(!tmp) break;
            size_t got_b = fread(tmp, 1, raw, sr->fp);
            size_t got_s = got_b / 3;
            size_t gf    = got_s / (size_t)spf;
            int16_t *out = buf + total*(size_t)spf;
            for(size_t s=0; s<got_s; s++){
                int32_t v = (int32_t)((uint32_t)tmp[s*3]
                           |((uint32_t)tmp[s*3+1]<<8)
                           |((uint32_t)tmp[s*3+2]<<16));
                if(v & 0x800000) v |= (int32_t)0xFF000000;
                out[s] = (int16_t)(v >> 8);
            }
            free(tmp);
            total += gf;
            if(gf < want){ fclose(sr->fp); sr->fp=NULL; sr->cur++; }
        }
    }
    return total;
}

static void seq_close(SeqReader *sr)
{
    if(sr->fp){ fclose(sr->fp); sr->fp=NULL; }
}

/* Seek sr to a frame position relative to the start of the whole sequence */
static int seq_seek_frame(SeqReader *sr, uint64_t frame,
                          const uint64_t *file_frames, int file_count)
{
    if(sr->fp){ fclose(sr->fp); sr->fp=NULL; }
    uint64_t acc=0;
    for(int i=0;i<file_count;i++){
        if(frame < acc + file_frames[i]){
            sr->cur = i;
            sr->fp  = _wfopen(sr->paths[i],L"rb");
            if(!sr->fp) return -1;
            int bps2 = (sr->bytes_per_samp>0) ? sr->bytes_per_samp : 2;
            int64_t byte_off = sr->data_offset + (int64_t)(frame-acc)*(int64_t)sr->in_ch*(int64_t)bps2;
            file_seek(sr->fp, byte_off, SEEK_SET);
            return 0;
        }
        acc += file_frames[i];
    }
    sr->cur = file_count; /* past end */
    return -1;
}

/* Called once per block in each copy loop. Blocks while g_pause is set,
 * waking periodically to recheck. Returns immediately if g_cancel becomes
 * set while paused, so a paused job can still be cancelled cleanly. */
static void wait_if_paused(void)
{
    while(g_pause && !g_cancel){
        Sleep(100);
    }
}

/* Write n_samps int16 values to fp, converting to 24-bit if bps==24.
 * Returns 0 on success, -1 on write error. */
static int write_samples_bps(FILE *fp, const int16_t *buf, size_t n_samps, int bps)
{
    if(bps == 16){
        if(fwrite(buf, sizeof(int16_t), n_samps, fp) != n_samps) return -1;
    } else {
        /* Expand int16 to 24-bit LE: shift left 8, fill low byte with 0 */
        uint8_t tmp[3];
        for(size_t i=0; i<n_samps; i++){
            int32_t v = (int32_t)buf[i] << 8;
            tmp[0] = (uint8_t)(v & 0xFF);
            tmp[1] = (uint8_t)((v>>8) & 0xFF);
            tmp[2] = (uint8_t)((v>>16) & 0xFF);
            if(fwrite(tmp, 1, 3, fp) != 3) return -1;
        }
    }
    return 0;
}

static int copy_ddc(SeqReader *sr, FILE *fout,
    uint64_t warmup_frames, uint64_t in_frames,
    int in_ch, int out_ch, int out_bps,
    const double *h, int n_taps, int D,
    double delta_phi, OutputChan ochan,
    const char *label)
{
    int do_b = (out_ch==4 || ochan==OUT_CH2);

    /* Convert prototype FIR to float for speed */
    float *hf = (float*)malloc(n_taps * sizeof(float));
    if(!hf){ proc_perror("malloc hf"); return -1; }
    for(int k=0; k<n_taps; k++) hf[k]=(float)h[k];

    /* Circular delay lines length n_taps — eliminates memmove per sample */
    float *dly_aI = (float*)calloc(n_taps, sizeof(float));
    float *dly_aQ = (float*)calloc(n_taps, sizeof(float));
    float *dly_bI = (float*)calloc(n_taps, sizeof(float));
    float *dly_bQ = (float*)calloc(n_taps, sizeof(float));
    int16_t *ibuf  = (int16_t*)malloc(COPY_BUF_FRAMES*(size_t)in_ch*2);
    int16_t *obuf  = (int16_t*)malloc(((size_t)COPY_BUF_FRAMES/D+4)*(size_t)out_ch*2);
    if(!dly_aI||!dly_aQ||!dly_bI||!dly_bQ||!ibuf||!obuf){
        proc_perror("malloc ddc");
        free(hf);free(dly_aI);free(dly_aQ);free(dly_bI);free(dly_bQ);
        free(ibuf);free(obuf); return -1;
    }

    /* Running phase rotation: dc+j*ds = e^{j*phi}, step by delta_phi each sample */
    double dc=1.0, ds=0.0;
    double dc_step=cos(delta_phi), ds_step=sin(delta_phi);
    int head=0, phase=0;

    /* ── Warmup: pre-fill delay line without writing output ──
     * Read warmup_frames of input that precede the trim start.
     * This populates the FIR delay line so the first output sample
     * is computed from real data, eliminating startup corruption. */
    uint64_t warmup_left = warmup_frames;
    while(warmup_left > 0){
        if(g_cancel) break;
        wait_if_paused();
        if(g_cancel) break;
        uint64_t batch=(warmup_left>(uint64_t)COPY_BUF_FRAMES)?
                        (uint64_t)COPY_BUF_FRAMES:warmup_left;
        size_t gf=seq_read_frames(sr,ibuf,batch);
        if(gf==0) break;
        for(int i=0;i<(int)gf;i++){
            float IA=(float)ibuf[i*in_ch+0], QA=(float)ibuf[i*in_ch+1];
            if(++head>=n_taps) head=0;
            dly_aI[head]=(float)(IA*dc-QA*ds);
            dly_aQ[head]=(float)(IA*ds+QA*dc);
            if(do_b){
                float IB=(float)ibuf[i*in_ch+2], QB=(float)ibuf[i*in_ch+3];
                dly_bI[head]=(float)(IB*dc-QB*ds);
                dly_bQ[head]=(float)(IB*ds+QB*dc);
            }
            double dc2=dc*dc_step-ds*ds_step;
            double ds2=dc*ds_step+ds*dc_step;
            dc=dc2; ds=ds2;
            if((i&4095)==0){double inv=1.0/sqrt(dc*dc+ds*ds);dc*=inv;ds*=inv;}
            if(++phase>=D) phase=0;
        }
        warmup_left-=(uint64_t)gf;
    }
    uint64_t left=in_frames, done=0;
    progress_print(label, 0, in_frames);

    while(left > 0){
        if(g_cancel) break;
        wait_if_paused();
        if(g_cancel) break;
        uint64_t batch=(left>(uint64_t)COPY_BUF_FRAMES)?
                        (uint64_t)COPY_BUF_FRAMES:left;
        size_t gf=seq_read_frames(sr,ibuf,batch);
        if(gf==0){ proc_log("\nWarning: source ended early.\n"); break; }
        int n_out=0;

        for(int i=0; i<(int)gf; i++){
            /* Frequency-shift and push into circular delay line */
            float IA=(float)ibuf[i*in_ch+0], QA=(float)ibuf[i*in_ch+1];
            if(++head >= n_taps) head=0;
            dly_aI[head]=(float)(IA*dc - QA*ds);
            dly_aQ[head]=(float)(IA*ds + QA*dc);
            if(do_b){
                float IB=(float)ibuf[i*in_ch+2], QB=(float)ibuf[i*in_ch+3];
                dly_bI[head]=(float)(IB*dc - QB*ds);
                dly_bQ[head]=(float)(IB*ds + QB*dc);
            }

            /* Advance running phase rotation */
            double dc2=dc*dc_step - ds*ds_step;
            double ds2=dc*ds_step + ds*dc_step;
            dc=dc2; ds=ds2;
            /* Renormalise every 4096 samples to prevent drift */
            if((i&4095)==0){
                double inv=1.0/sqrt(dc*dc+ds*ds);
                dc*=inv; ds*=inv;
            }

            /* Decimate: compute full FIR output every D input samples */
            if(phase==0){
                /* Apply all n_taps coefficients using circular buffer */
                float oIA=0, oQA=0;
                int p=head;
                for(int k=0; k<n_taps; k++){
                    oIA+=hf[k]*dly_aI[p];
                    oQA+=hf[k]*dly_aQ[p];
                    if(--p<0) p=n_taps-1;
                }
                if(out_ch==4){
                    float oIB=0,oQB=0; p=head;
                    for(int k=0;k<n_taps;k++){
                        oIB+=hf[k]*dly_bI[p]; oQB+=hf[k]*dly_bQ[p];
                        if(--p<0)p=n_taps-1;
                    }
                    obuf[n_out*4+0]=CLIP16(oIA); obuf[n_out*4+1]=CLIP16(oQA);
                    obuf[n_out*4+2]=CLIP16(oIB); obuf[n_out*4+3]=CLIP16(oQB);
                } else if(ochan==OUT_CH2){
                    float oIB=0,oQB=0; p=head;
                    for(int k=0;k<n_taps;k++){
                        oIB+=hf[k]*dly_bI[p]; oQB+=hf[k]*dly_bQ[p];
                        if(--p<0)p=n_taps-1;
                    }
                    obuf[n_out*2+0]=CLIP16(oIB); obuf[n_out*2+1]=CLIP16(oQB);
                } else {
                    obuf[n_out*2+0]=CLIP16(oIA); obuf[n_out*2+1]=CLIP16(oQA);
                }
                n_out++;
            }
            if(++phase >= D) phase=0;
        }

        if(n_out>0){
            size_t samps=(size_t)n_out*(size_t)out_ch;
            if(write_samples_bps(fout,obuf,samps,out_bps)!=0){
                proc_perror("\nfwrite");
                free(hf);free(dly_aI);free(dly_aQ);free(dly_bI);free(dly_bQ);
                free(ibuf);free(obuf); return -1;
            }
        }
        done+=gf; left-=gf;
        progress_print(label,done,in_frames);
    }
    free(hf);free(dly_aI);free(dly_aQ);free(dly_bI);free(dly_bQ);
    free(ibuf);free(obuf);
    progress_done();
    return g_cancel ? 2 : 0;
}

/* ------------------------------------------------------------------ */
/*  Streaming copy (no DDC, no resampling)                             */
/* ------------------------------------------------------------------ */
static int copy_passthrough(SeqReader *sr, FILE *fout,
    uint64_t total_frames, int in_ch, int out_ch, int out_bps,
    int src_idx, /* 0=chA IQ, 2=chB IQ, -1=all channels */
    const char *label)
{
    int16_t *ibuf=(int16_t*)malloc(COPY_BUF_FRAMES*(size_t)in_ch*2);
    int16_t *obuf=(int16_t*)malloc(COPY_BUF_FRAMES*(size_t)out_ch*2);
    if(!ibuf||!obuf){ proc_perror("malloc"); free(ibuf);free(obuf); return -1; }

    uint64_t left=total_frames, done=0;
    progress_print(label,0,total_frames);
    while(left>0){
        if(g_cancel) break;
        wait_if_paused();
        if(g_cancel) break;
        uint64_t batch=(left>(uint64_t)COPY_BUF_FRAMES)?(uint64_t)COPY_BUF_FRAMES:left;
        size_t gf=seq_read_frames(sr,ibuf,batch);
        if(gf==0){ proc_log("\nWarning: source ended early.\n"); break; }
        if(src_idx<0){
            if(write_samples_bps(fout,ibuf,gf*(size_t)in_ch,out_bps)!=0){
                proc_perror("\nfwrite"); free(ibuf);free(obuf); return -1; }
        } else {
            for(size_t f=0;f<gf;f++){
                obuf[f*2+0]=ibuf[f*in_ch+src_idx];
                obuf[f*2+1]=ibuf[f*in_ch+src_idx+1];
            }
            if(write_samples_bps(fout,obuf,gf*2,out_bps)!=0){
                proc_perror("\nfwrite"); free(ibuf);free(obuf); return -1; }
        }
        done+=(uint64_t)gf; left-=(uint64_t)gf;
        progress_print(label,done,total_frames);
    }
    free(ibuf);free(obuf);
    progress_done();
    return g_cancel ? 2 : 0;
}

/* ================================================================== */
/*  Polyphase Rational Resampler                                       */
/* ================================================================== */

typedef struct {
    int      P;              /* upsample factor   */
    int      Q;              /* downsample factor */
    int      tps;            /* taps per subfilter */
    float  **sub;            /* sub[p] -> contiguous block sub[0]+p*tps */
    float   *dly_I;          /* IQ circular delay lines */
    float   *dly_Q;
    int      phase;          /* current subfilter index 0..P-1 */
    int      dly_pos;        /* circular buffer head */
} Resampler;

static Resampler *resamp_create(int fs_in, int fs_out, float atten_db)
{
    int a=fs_in, b=fs_out, g;
    while(b){int t2=b; b=a%b; a=t2;} g=a;
    int P=fs_out/g, Q=fs_in/g;

    double fc  = 0.45 * (P < Q ? (double)P : (double)Q)
                      / ((double)P * (double)(P > Q ? P : Q));
    double df  = fc * 0.1;
    double beta = (atten_db>=50.f)?0.1102*(atten_db-8.7):
                  (atten_db>=21.f)?0.5842*pow(atten_db-21.0,0.4)+
                                   0.07886*(atten_db-21.0):0.0;
    int total = (int)ceil((atten_db-8.0)/(2.285*2.0*M_PI*df));
    total = ((total+P-1)/P)*P;
    if(total < P*4) total = P*4;
    int tps = total/P;

    double *proto = (double*)malloc((size_t)total*sizeof(double));
    if(!proto) return NULL;
    double I0b = bessel_i0(beta);
    int M = total-1;
    for(int n=0;n<total;n++){
        double nt = (double)n - M*0.5;
        double sinc = (fabs(nt)<1e-10)?2.0*fc:sin(2*M_PI*fc*nt)/(M_PI*nt);
        double rr = (M>0)?(2.0*((double)n-M*0.5)/M):0.0;
        double w = bessel_i0(beta*sqrt(fabs(1.0-rr*rr)))/I0b;
        proto[n] = sinc*w;
    }
    double sum=0; for(int n=0;n<total;n++) sum+=proto[n];
    double scale = (sum>1e-10)?(double)P/sum:1.0;

    Resampler *rs=(Resampler*)calloc(1,sizeof(Resampler));
    if(!rs){free(proto);return NULL;}
    rs->P=P; rs->Q=Q; rs->tps=tps;

    float *sub_data=(float*)calloc((size_t)P*tps,sizeof(float));
    rs->sub=(float**)malloc((size_t)P*sizeof(float*));
    if(!sub_data||!rs->sub){
        free(sub_data);free(rs->sub);free(proto);free(rs);return NULL;
    }
    for(int p=0;p<P;p++){
        rs->sub[p]=sub_data+p*tps;
        for(int t=0;t<tps;t++){
            int idx=p+t*P;
            rs->sub[p][t]=(idx<total)?(float)(proto[idx]*scale):0.0f;
        }
    }
    free(proto);

    rs->dly_I=(float*)calloc((size_t)tps,sizeof(float));
    rs->dly_Q=(float*)calloc((size_t)tps,sizeof(float));
    if(!rs->dly_I||!rs->dly_Q){
        free(sub_data);free(rs->sub);
        free(rs->dly_I);free(rs->dly_Q);free(rs);return NULL;
    }
    rs->phase=0; rs->dly_pos=0;
    return rs;
}

static void resamp_free(Resampler *r)
{
    if(!r) return;
    if(r->sub){ if(r->sub[0]) free(r->sub[0]); free(r->sub); }
    free(r->dly_I);free(r->dly_Q);free(r);
}

static int resamp_process_block(Resampler *rs,
    const int16_t *in, int in_frames,
    int16_t *out, int out_max)
{
    int P=rs->P, Q=rs->Q, tps=rs->tps;
    int out_count=0, in_idx=0;

    while(out_count < out_max){
        while(rs->phase >= P){
            rs->phase -= P;
            if(in_idx >= in_frames) return out_count;
            if(++(rs->dly_pos) >= tps) rs->dly_pos = 0;
            rs->dly_I[rs->dly_pos] = (float)in[in_idx*2+0];
            rs->dly_Q[rs->dly_pos] = (float)in[in_idx*2+1];
            in_idx++;
        }
        const float *h = rs->sub[rs->phase];
        float oI=0.0f, oQ=0.0f;
        int dp = rs->dly_pos;
        for(int t=0; t<tps; t++){
            oI += h[t] * rs->dly_I[dp];
            oQ += h[t] * rs->dly_Q[dp];
            if(--dp < 0) dp = tps-1;
        }
        int vi=(int)(oI+0.5f), vq=(int)(oQ+0.5f);
        if(vi >  32767) vi =  32767; else if(vi < -32768) vi = -32768;
        if(vq >  32767) vq =  32767; else if(vq < -32768) vq = -32768;
        out[out_count*2+0] = (int16_t)vi;
        out[out_count*2+1] = (int16_t)vq;
        out_count++;
        rs->phase += Q;
    }
    return out_count;
}

static int copy_resample(SeqReader *sr, FILE *fout,
    uint64_t in_frames, int in_ch, int out_ch, int out_bps,
    int fs_in, int fs_out,
    OutputChan ochan, const char *label)
{
    /* Extract one channel pair (src_idx) from potentially multi-ch input */
    int src_idx = (ochan==OUT_CH2)?2:(ochan==OUT_CH1)?0:0;

    proc_log("Creating resampler %d->%d Hz...\n", fs_in, fs_out);
    Resampler *rs = resamp_create(fs_in, fs_out, 80.0f);
    if(!rs){ proc_log("Error: resampler allocation failed\n"); return -1; }

    proc_log("Resampler: P=%d Q=%d tps=%d (%.1f MB)\n",
             rs->P, rs->Q, rs->tps,
             (double)rs->P*rs->tps*4/(1024*1024));

    int BLOCK_BASE = 65536;
    /* Round block size down to multiple of Q to ensure clean block boundaries */
    int BLOCK = (BLOCK_BASE / rs->Q) * rs->Q;
    if(BLOCK < rs->Q) BLOCK = rs->Q;
    int16_t *ibuf = (int16_t*)malloc((size_t)BLOCK * in_ch * 2);
    int16_t *mono = (int16_t*)malloc((size_t)BLOCK * 2 * 2); /* extracted IQ */
    /* Output buffer: worst case P/Q * BLOCK + margin */
    int out_block = (int)((double)BLOCK * rs->P / rs->Q + 256);
    int16_t *obuf = (int16_t*)malloc((size_t)out_block * out_ch * 2);
    if(!ibuf||!mono||!obuf){
        proc_perror("malloc");
        free(ibuf);free(mono);free(obuf);resamp_free(rs);return -1;
    }

    uint64_t left=in_frames, done=0;
    int block_num=0;
    progress_print(label,0,in_frames);

    while(left>0){
        if(g_cancel) break;
        wait_if_paused();
        if(g_cancel) break;
        uint64_t batch=(left>(uint64_t)BLOCK)?(uint64_t)BLOCK:left;
        size_t gf_s=seq_read_frames(sr,ibuf,batch);
        if(gf_s==0){proc_log("\nWarning: source ended early.\n");break;}
        int gf=(int)gf_s;
        block_num++;

        /* Extract channel */
        for(int i=0;i<gf;i++){
            mono[i*2+0]=ibuf[i*in_ch+src_idx];
            mono[i*2+1]=ibuf[i*in_ch+src_idx+1];
        }

        /* Resample */
        int n_out=resamp_process_block(rs,mono,(int)gf,obuf,out_block);
        if(n_out < 0 || n_out > out_block){
            proc_log("Error: resamp_process_block returned %d (out_block=%d)\n",
                     n_out, out_block);
            free(ibuf);free(mono);free(obuf);resamp_free(rs);return -1;
        }

        if(n_out>0){
            size_t samps=(size_t)n_out*out_ch;
            if(write_samples_bps(fout,obuf,samps,out_bps)!=0){
                proc_perror("\nfwrite");
                free(ibuf);free(mono);free(obuf);resamp_free(rs);return -1;
            }
        }
        done+=(uint64_t)gf; left-=(uint64_t)gf;
        progress_print(label,done,in_frames);
    }
    free(ibuf);free(mono);free(obuf);resamp_free(rs);
    progress_done();
    return g_cancel ? 2 : 0;
}

/* ------------------------------------------------------------------ */
/*  Print usage                                                        */
/* ------------------------------------------------------------------ */
static int sdr_process_args(int argc, char *argv[])
{
    if(argc<3){ /* usage not shown in GUI */; return 1; }

    const char *inpath = argv[1];

    /* ── Parse positional args and options ── */
    int      do_convert  = 0;
    int      req_start_hh=0,req_start_mm=0,req_start_ss=0;
    int      req_end_hh=0,  req_end_mm=0,  req_end_ss=0;
    OutputChan ochan     = OUT_DUAL;     /* default: keep all channels */
    FileFormat out_fmt   = FMT_UNKNOWN;  /* resolved after input detect */
    int      do_ddc      = 0;
    int      ddc_freq_khz= 0;
    int      ddc_bw_khz  = 0;
    int      ochan_set   = 0;           /* user explicitly set ch1/ch2 */
    int      out_bps_override = 0;      /* 0=auto, 16 or 24=forced via --24bit */
    char     outdir_override[512]={0}; /* custom output directory via --outdir */

    /* Second arg: HHMMSS start time, --convert, or start of options (implicit full-file) */
    if(strcmp(argv[2],"--convert")==0){
        do_convert=1;
    } else if(strlen(argv[2])==6
              && sscanf(argv[2],"%2d%2d%2d",&req_start_hh,&req_start_mm,&req_start_ss)==3
              && req_start_hh<=23 && req_start_mm<=59 && req_start_ss<=59){
        /* argv[2] looks like a valid HHMMSS time  -  argv[3] must also be valid HHMMSS */
        if(argc<4){
            proc_log("Error: start time %s given but no end time.\n"
                    "  Usage: sdrtrim <input> <start_HHMMSS> <end_HHMMSS> [options]\n",
                    argv[2]);
            return 1;
        }
        if(strlen(argv[3])!=6
           || sscanf(argv[3],"%2d%2d%2d",&req_end_hh,&req_end_mm,&req_end_ss)!=3
           || req_end_hh>23 || req_end_mm>59 || req_end_ss>59){
            proc_log("Error: start time %s given but end time '%s' is not valid HHMMSS.\n"
                    "  Both start and end must be 6-digit times, e.g. 030000 120000.\n"
                    "  For full-file processing omit both time arguments.\n",
                    argv[2], argv[3]);
            return 1;
        }
        /* Trim mode confirmed */
    } else {
        /* No HHMMSS and no --convert: implicit full-file mode.
         * argv[2] onwards are options (--ch1, --ch2, format word, --ddc, etc.) */
        do_convert=1;
    }

    /* opt_start: where options begin in argv.
     * Trim mode:              argv[2]=HHMM  argv[3]=HHMM  options from [4]
     * --convert explicit:     argv[2]=--convert            options from [3]
     * --convert implicit:     argv[2] is already an option, options from [2] */
    int opt_start;
    if(!do_convert){
        opt_start = 4;   /* trim mode */
    } else if(strcmp(argv[2],"--convert")==0){
        opt_start = 3;   /* explicit --convert */
    } else {
        opt_start = 2;   /* implicit: argv[2] is first option */
    }
    for(int i=opt_start;i<argc;i++){
        if(strcmp(argv[i],"--ch1")==0){ ochan=OUT_CH1; ochan_set=1; }
        else if(strcmp(argv[i],"--ch2")==0){ ochan=OUT_CH2; ochan_set=1; }
        else if(strcmp(argv[i],"--seq")==0){ i++; /* handled later in SeqReader build */ }
        else if(strcmp(argv[i],"--24bit")==0){ out_bps_override=24; }
        else if(strcmp(argv[i],"--16bit")==0){ out_bps_override=16; }
        else if(strcmp(argv[i],"--outdir")==0 && i+1<argc){
            i++;
            snprintf(outdir_override,sizeof(outdir_override),"%s",argv[i]);
            /* Ensure trailing backslash */
            size_t dl=strlen(outdir_override);
            if(dl>0 && outdir_override[dl-1]!='\\' && outdir_override[dl-1]!='/')
                strcat(outdir_override,"\\");
        }
        else if(strcmp(argv[i],"linrad"    )==0) out_fmt=FMT_LINRAD;
        else if(strcmp(argv[i],"wavviewdx" )==0) out_fmt=FMT_WAVVIEWDX;
        else if(strcmp(argv[i],"sdruno"    )==0) out_fmt=FMT_SDRUNO;
        else if(strcmp(argv[i],"sdrconnect")==0) out_fmt=FMT_SDRCONNECT;
        else if(strcmp(argv[i],"perseus"   )==0) out_fmt=FMT_PERSEUS;
        else if(strcmp(argv[i],"jaguar"    )==0) out_fmt=FMT_JAGUAR;
        else if(strcmp(argv[i],"--fmt")==0){
            /* --fmt <format> accepted for backward compatibility */
            if(i+1>=argc){ proc_log("Error: --fmt requires a format name\n");
                return 1; }
            i++;
            if     (strcmp(argv[i],"linrad"    )==0) out_fmt=FMT_LINRAD;
            else if(strcmp(argv[i],"wavviewdx" )==0) out_fmt=FMT_WAVVIEWDX;
            else if(strcmp(argv[i],"sdruno"    )==0) out_fmt=FMT_SDRUNO;
            else if(strcmp(argv[i],"sdrconnect")==0) out_fmt=FMT_SDRCONNECT;
            else if(strcmp(argv[i],"perseus"   )==0) out_fmt=FMT_PERSEUS;
            else if(strcmp(argv[i],"jaguar"    )==0) out_fmt=FMT_JAGUAR;
            else{ proc_log("Error: unknown format '%s'\n",argv[i]);
                  return 1; }
        }
        else if(strcmp(argv[i],"--ddc")==0){
            if(i+2>=argc){ proc_log("Error: --ddc requires <kHz> <bw_kHz>\n");
                return 1; }
            do_ddc=1;
            ddc_freq_khz=atoi(argv[i+1]);
            ddc_bw_khz  =atoi(argv[i+2]);
            if(ddc_freq_khz<=0||ddc_bw_khz<=0){
                proc_log("Error: DDC frequencies must be positive integers\n");
                return 1; }
            i+=2;
        }
        else if(strcmp(argv[i],"--convert")==0){ /* already handled */ }
        else{ proc_log("Error: unrecognised argument '%s'\n"
                   "  Expected: HHMMSS time, format word (linrad/wavviewdx/sdruno/sdrconnect),\n"
                   "  --ch1, --ch2, --ddc <kHz> <bw>, or --convert\n",argv[i]);
              return 1; }
    }

    /* ── Open input ── */
    FILE *fin=fopen(inpath,"rb");
    if(!fin){ proc_log("Error opening '%s': %s\n",inpath,strerror(errno));
              return 1; }

    RecInfo rec;
    LinradHdr linrad_hdr;
    memset(&linrad_hdr,0,sizeof(linrad_hdr));
    if(!detect_input(inpath,&rec,fin,&linrad_hdr)){
        fclose(fin); return 1; }

    /* ── Resolve output channel mode ── */
    if(!ochan_set){
        /* No --ch1/--ch2: default to dual if input is dual, single otherwise */
        ochan = (rec.tuner==TUNER_DUAL) ? OUT_DUAL : OUT_CH1;
    }

    /* Validate: can't request ch2 from single-tuner input */
    if(ochan==OUT_CH2 && rec.tuner==TUNER_SINGLE){
        proc_log(
            "Error: --ch2 requested but input is single-tuner (no second tuner).\n");
        fclose(fin); return 1;
    }

    /* Validate: can't request dual output from single-tuner input */
    if(ochan==OUT_DUAL && rec.tuner==TUNER_SINGLE){
        /* Silently treat as ch1 for single-tuner */
        ochan=OUT_CH1;
    }

    /* ── Resolve output format ── */
    int out_is_dual=(ochan==OUT_DUAL);
    if(out_fmt==FMT_UNKNOWN){
        /* Default to the input format where compatible; fall back when not. */
        if(out_is_dual){
            /* Dual output: WAV formats not supported  -  use input fmt or linrad */
            out_fmt = (rec.fmt==FMT_WAVVIEWDX) ? FMT_WAVVIEWDX : FMT_LINRAD;
        } else {
            /* Single output: use input format if it supports single-tuner, else sdruno */
            out_fmt = (rec.fmt==FMT_UNKNOWN) ? FMT_SDRUNO : rec.fmt;
        }
    }

    /* Validate: WAV formats not allowed for dual-tuner output */
    if(out_is_dual && (out_fmt==FMT_SDRUNO||out_fmt==FMT_SDRCONNECT||
                       out_fmt==FMT_PERSEUS||out_fmt==FMT_JAGUAR)){
        proc_log(
            "Error: dual-tuner output only supports linrad and wavviewdx formats.\n"
            "  Use --ch1 or --ch2 to extract a single tuner for WAV output.\n");
        fclose(fin); return 1;
    }

    /* ── Format name strings (used in messages below) ── */
    const char *fmt_names[]={"Linrad raw","WavViewDX raw","SDRuno WAV",
                              "SDR Connect WAV","Perseus WAV","Jaguar WAV","Unknown"};

    /* Validate: same input and output format with no meaningful operation.
     *
     * Same format is valid when any of the following apply:
     *   - time trimming is applied (do_convert == 0)
     *   - DDC is applied
     *   - a channel is being extracted from a dual-tuner input
     *
     * If same format + channel extraction only: proceed but note no format
     * conversion is occurring.
     *
     * If truly nothing to do (same format, no trim, no DDC, no extraction):
     * print an informational message and exit cleanly. */
    if(do_convert && !do_ddc && out_fmt == rec.fmt){
        if(rec.tuner == TUNER_DUAL && ochan != OUT_DUAL){
            /* Channel extraction from dual to single  -  valid, no message needed */
        } else {
            proc_log("Note: input and output are the same format (%s) with no trim, DDC,\n"
                   "  or channel extraction  -  no conversion required.\n",
                   fmt_names[rec.fmt]);
            fclose(fin); return 0;
        }
    }

    /* ── Print input info ── */
    proc_log("Input       : %s\n",inpath);
    proc_log("Format      : %s\n",fmt_names[rec.fmt]);
    proc_log("Tuner       : %s\n",rec.tuner==TUNER_DUAL?"Dual (IA,QA,IB,QB)":"Single (I,Q)");
    proc_log("Sample rate : %d Hz\n",rec.sample_rate);
    proc_log("Bits/sample : %d\n", rec.bits_per_sample>0 ? rec.bits_per_sample : 16);
    proc_log("Centre freq : %d Hz = %.3f kHz\n",rec.centre_freq_hz,rec.centre_freq_hz/1000.0);
    proc_log("Start time  : %04d-%02d-%02d %02d:%02d:%02d UTC\n",
           rec.year,rec.month,rec.day,rec.hour,rec.minute,rec.second);

    int bytes_per_samp = (rec.bits_per_sample > 0) ? (rec.bits_per_sample/8) : 2;
    uint64_t total_frames_in_file = rec.data_bytes / ((uint64_t)rec.num_channels*(uint64_t)bytes_per_samp);
    proc_log("Data bytes  : %llu  bps=%d  ch=%d  -> %llu frames\n",
             (unsigned long long)rec.data_bytes, bytes_per_samp,
             rec.num_channels, (unsigned long long)total_frames_in_file);
    uint64_t total_secs=total_frames_in_file/(uint64_t)rec.sample_rate;
    proc_log("Duration    : %llu s\n",(unsigned long long)total_secs);
    proc_log("Output mode : %s -> %s\n",
           ochan==OUT_DUAL?"Dual-tuner":ochan==OUT_CH1?"Tuner A (ch1)":"Tuner B (ch2)",
           fmt_names[out_fmt]);

    /* ── Resolve trim / convert times ── */
    int64_t file_epoch = rec.epoch;
    int out_year=rec.year, out_month=rec.month, out_day=rec.day;
    int64_t start_epoch, end_epoch;

    if(do_convert){
        /* No trim — use exact frame count, not rounded seconds */
        start_epoch = file_epoch;
        end_epoch   = file_epoch + (int64_t)total_secs;
        out_year=rec.year; out_month=rec.month; out_day=rec.day;
    } else {
        /* Resolve start */
        start_epoch=utc_to_unix(out_year,out_month,out_day,req_start_hh,req_start_mm,req_start_ss);
        if(start_epoch<file_epoch){
            next_day(&out_year,&out_month,&out_day);
            start_epoch=utc_to_unix(out_year,out_month,out_day,req_start_hh,req_start_mm,req_start_ss);
            proc_log("Note: start %02d:%02d:%02d past midnight  -  using %04d-%02d-%02d\n",
                   req_start_hh,req_start_mm,req_start_ss,out_year,out_month,out_day);
        }
        if(start_epoch<file_epoch){
            proc_log("Error: start time is before file start.\n");
            fclose(fin); return 1; }

        /* Resolve end */
        int end_year=out_year, end_month=out_month, end_day=out_day;
        end_epoch=utc_to_unix(end_year,end_month,end_day,req_end_hh,req_end_mm,req_end_ss);
        if(end_epoch<=start_epoch){
            next_day(&end_year,&end_month,&end_day);
            end_epoch=utc_to_unix(end_year,end_month,end_day,req_end_hh,req_end_mm,req_end_ss);
            proc_log("Note: end %02d:%02d <= start  -  using %04d-%02d-%02d\n",
                   req_end_hh,req_end_mm,end_year,end_month,end_day);
        }

        int64_t file_end=file_epoch+(int64_t)total_secs;
        if(end_epoch>file_end){
            int fe_hh,fe_mm,fe_ss,fe_yy,fe_mo,fe_dd;
            unix_to_utc(file_end,&fe_yy,&fe_mo,&fe_dd,&fe_hh,&fe_mm,&fe_ss);
            proc_log("Error: end time %02d:%02d is beyond the end of the recording.\n"
                    "  Recording ends at %02d:%02d UTC on %04d-%02d-%02d.\n",
                    req_end_hh,req_end_mm,fe_hh,fe_mm,fe_yy,fe_mo,fe_dd);
            fclose(fin); return 1; }
    }

    uint64_t start_frame =(uint64_t)(start_epoch-file_epoch)*(uint64_t)rec.sample_rate;
    uint64_t in_frames;
    if(do_convert){
        /* Full file: use exact frame count to avoid losing fractional seconds */
        in_frames = total_frames_in_file - start_frame;
    } else {
        int64_t duration_sec  = end_epoch - start_epoch;
        if(duration_sec<=0){
            proc_log("Error: zero or negative duration.\n");
            fclose(fin); return 1; }
        in_frames = (uint64_t)duration_sec*(uint64_t)rec.sample_rate;
    }

    /* ── DDC setup ── */
    int    ddc_D=1, fs_out=rec.sample_rate;
    double delta_phi=0.0;
    int    ddc_cf_hz=rec.centre_freq_hz;
    double *fir_h=NULL;
    int    n_taps=0;

    if(do_ddc){
        ddc_D  = rec.sample_rate/(ddc_bw_khz*1000);
        if(ddc_D<1) ddc_D=1;
        fs_out = rec.sample_rate/ddc_D;
        ddc_cf_hz = ddc_freq_khz*1000;

        double cutoff_norm=(ddc_bw_khz*1000.0/2.0)/rec.sample_rate;
        fir_h=design_fir(cutoff_norm,cutoff_norm*0.15,80.0,&n_taps);
        if(!fir_h){ proc_perror("malloc FIR"); fclose(fin); return 1; }

        delta_phi=2.0*M_PI*(double)(rec.centre_freq_hz-ddc_cf_hz)/(double)rec.sample_rate;

        proc_log("DDC centre  : %d kHz\n",ddc_freq_khz);
        proc_log("DDC BW      : %d kHz\n",ddc_bw_khz);
        proc_log("Decimation  : %d -> output rate %d Hz\n",ddc_D,fs_out);
        proc_log("FIR taps    : %d\n",n_taps);
    }

    uint64_t out_frames = in_frames/(uint64_t)(do_ddc?ddc_D:1);

    /* ── Output channel count ── */
    int out_ch = (ochan==OUT_DUAL) ? rec.num_channels : 2;

    /* Trim info */
    int trim_sh=req_start_hh, trim_sm=req_start_mm, trim_ss=req_start_ss;
    int trim_eh=req_end_hh,   trim_em=req_end_mm;
    if(do_convert){
        trim_sh=rec.hour; trim_sm=rec.minute; trim_ss=rec.second;
        int64_t e2=file_epoch+(int64_t)total_secs;
        int ey,em,ed,eh,emi,es;
        unix_to_utc(e2,&ey,&em,&ed,&eh,&emi,&es);
        trim_eh=eh; trim_em=emi;
    }

    int end_year2=out_year, end_month2=out_month, end_day2=out_day;
    unix_to_utc(end_epoch,&end_year2,&end_month2,&end_day2,
                &trim_eh,&trim_em,
                &(int){0});

    /* ── Build output filename ── */
    /* Extract input file directory to place output alongside input */
    char outdir[512]={0};
    {
        const char *sl=strrchr(inpath,'\\');
        if(!sl) sl=strrchr(inpath,'/');
        if(sl){ size_t dlen=(size_t)(sl-inpath)+1;
                if(dlen<sizeof(outdir)){ memcpy(outdir,inpath,dlen); outdir[dlen]='\0'; } }
    }
    char outbase[512];
    char outpath[512];
    switch(out_fmt){
        case FMT_LINRAD:
            build_linrad_filename(outbase,sizeof(outbase),
                out_year,out_month,out_day,
                trim_sh,trim_sm,trim_ss,
                rec.utc_offset,ddc_cf_hz,out_ch);
            break;
        case FMT_WAVVIEWDX:
            build_wavviewdx_filename(outbase,sizeof(outbase),
                out_year,out_month,out_day,
                trim_sh,trim_sm,trim_ss,
                ddc_cf_hz,fs_out,out_ch);
            /* append .raw */
            { size_t n=strlen(outbase);
              if(n+4<sizeof(outbase)) strcat(outbase,".raw"); }
            break;
        case FMT_SDRUNO:
            build_sdruno_filename(outbase,sizeof(outbase),
                out_year,out_month,out_day,
                trim_sh,trim_sm,trim_ss,
                ddc_cf_hz);
            break;
        case FMT_SDRCONNECT:
            build_sdrconnect_filename(outbase,sizeof(outbase),
                out_year,out_month,out_day,
                trim_sh,trim_sm,trim_ss,
                ddc_cf_hz);
            break;
        case FMT_PERSEUS:
        case FMT_JAGUAR:
            build_rcvr_filename(outbase,sizeof(outbase),
                out_year,out_month,out_day,
                trim_sh,trim_sm,trim_ss,
                ddc_cf_hz,out_fmt);
            break;
        default: outbase[0]='\0'; break;
    }
    if(outdir_override[0])
        snprintf(outpath,sizeof(outpath),"%s%s",outdir_override,outbase);
    else
        snprintf(outpath,sizeof(outpath),"%s%s",outdir,outbase);

    /* Prevent input overwrite.
     * If the computed output name matches the input, append _cvt before
     * the extension so the original is never overwritten or deleted. */
    {
        const char *ib=file_basename(inpath);
        const char *ob=file_basename(outpath);
        if(strcmp(ib,ob)==0){
            char tmp[512];
            char *dot=strrchr(outpath,'.');
            if(dot){
                size_t blen=(size_t)(dot-outpath);
                if(blen+9<sizeof(tmp)){
                    memcpy(tmp,outpath,blen); tmp[blen]='\0';
                    strncat(tmp,"_cvt",sizeof(tmp)-blen-1);
                    strncat(tmp,dot,sizeof(tmp)-strlen(tmp)-1);
                    snprintf(outpath,sizeof(outpath),"%s",tmp);
                }
            } else {
                size_t n=strlen(outpath);
                if(n+4<sizeof(outpath)) strcat(outpath,"_cvt");
            }
            proc_log("Note: output name matched input -- renamed to: %s\n",outpath);
        }
    }

    /* Check if output file already exists — ask user via main thread */
    {
        FILE *ftest=fopen(outpath,"rb");
        if(ftest){
            fclose(ftest);
            wchar_t *wpath=(wchar_t*)malloc(512*sizeof(wchar_t));
            if(wpath){
                MultiByteToWideChar(CP_UTF8,0,outpath,-1,wpath,512);
                /* SendMessage is synchronous — blocks until user responds */
                LRESULT r=SendMessageW(g_proc_hwnd,WM_CONFIRM_OW,0,(LPARAM)wpath);
                /* wpath is freed by the handler */
                if(r!=IDYES){
                    proc_log("Cancelled — output file already exists.\n");
                    fclose(fin); free(fir_h); return 1;
                }
            }
        }
    }
    proc_log("Output file : %s\n\n",outpath);

    /* ── Build SeqReader from main input + any --seq files ── */
    SeqReader sr; memset(&sr,0,sizeof(sr));
    /* First file is always inpath (already open as fin) */
    /* inpath is char* - convert to wchar_t */
    MultiByteToWideChar(CP_UTF8,0,inpath,-1,sr.paths[0],MAX_PATH);
    sr.count=1; sr.in_ch=rec.num_channels; sr.sample_rate=rec.sample_rate;
    sr.data_offset=rec.data_offset; sr.cur=0; sr.fp=NULL;
    sr.bytes_per_samp = (rec.bits_per_sample>0) ? (rec.bits_per_sample/8) : 2;
    /* Add sequence files from global g_seq_paths (set by GUI) */
    for(int si=0;si<g_seq_count && sr.count<SEQ_MAX;si++){
        wcsncpy(sr.paths[sr.count],g_seq_paths[si],MAX_PATH-1);
        proc_log("Adding seq: '%ls'\n", sr.paths[sr.count]);
        sr.count++;
    }
    proc_log("Sequence    : %d file(s)\n",sr.count);
    /* Total frames across all files in sequence */
    uint64_t file_frames[SEQ_MAX]={0};
    file_frames[0] = total_frames_in_file; /* main file */
    uint64_t seq_total_frames = file_frames[0];
    for(int si=1;si<sr.count;si++){
        /* Use same data_offset as first file (same format/header size) */
        FILE *sf=_wfopen(sr.paths[si],L"rb");
        if(sf){
            _fseeki64(sf,0,SEEK_END);
            int64_t fsz=_ftelli64(sf); fclose(sf);
            int64_t db=fsz-rec.data_offset;
            if(db>0) file_frames[si]=(uint64_t)db/((uint64_t)rec.num_channels*(uint64_t)bytes_per_samp);
        } else {
            proc_log("Seq file %d : _wfopen failed errno=%d path='%ls'\n",si,errno,sr.paths[si]);
        }
        seq_total_frames += file_frames[si];
        proc_log("Seq file %d : %ls (%llu frames)\n",si,sr.paths[si],(unsigned long long)file_frames[si]);
    }
    fclose(fin); fin=NULL; /* SeqReader manages file handles from here */

    /* For full-file/convert with sequence, extend in_frames to cover all files */
    if(do_convert && sr.count > 1){
        uint64_t avail = seq_total_frames - start_frame;
        if(avail > in_frames) in_frames = avail;
    }

    /* ── Seek to trim start within sequence ── */

    if(seq_seek_frame(&sr,start_frame,file_frames,sr.count)!=0){
        proc_log("Error seeking to start frame.\n");
        free(fir_h); return 1; }

    /* ── Open output ── */
    FILE *fout=fopen(outpath,"wb");
    if(!fout){
        proc_log("Error creating '%s': %s\n",outpath,strerror(errno));
        free(fir_h); seq_close(&sr); return 1; }

    /* ── Write output header ── */
    int64_t ds64_pos=-1, data_sz32_pos=-1;
    int use_rf64=0;
    /* Determine resampling need — Jaguar always outputs at 1,600,000 Hz except
     * when converting to/from Perseus at 2,000,000 Hz (no resampling needed).
     * Perseus outputs at its native recorded rate (no rate conversion). */
    int need_resamp = 0;
    int resamp_target = fs_out;
    if(!do_ddc){
        if(out_fmt==FMT_JAGUAR && rec.sample_rate!=1600000
           && rec.sample_rate!=2000000){
            /* Resample to 1,600,000 Hz only when input is not a Jaguar-native rate.
             * At 1,600,000 and 2,000,000 Hz, pass through directly. */
            need_resamp=1; resamp_target=1600000;
        }
        /* Perseus: no rate conversion — always write at the input sample rate */
    }

    /* Determine output bits per sample — needs need_resamp/resamp_target to be set first */
    int out_bps = 16;
    if(out_fmt==FMT_PERSEUS){
        uint32_t out_sr = need_resamp ? (uint32_t)resamp_target : (uint32_t)fs_out;
        out_bps = (out_sr < 2000000) ? 24 : 16;
        if(out_bps_override==16 || out_bps_override==24)
            out_bps = out_bps_override;
        proc_log("Output bits  : %d-bit%s\n", out_bps,
                 out_bps_override ? " (user override)" : " (auto)");
    }

    /* Adjust frame count for resampling */
    uint64_t out_frames_actual = out_frames;
    if(need_resamp && rec.sample_rate > 0)
        out_frames_actual = (uint64_t)((double)out_frames * (double)resamp_target
                             / (double)rec.sample_rate + 0.5);
    uint64_t out_data_bytes=out_frames_actual*(uint64_t)out_ch*(uint64_t)(out_bps/8);

    /* Perseus/Jaguar: check output won't exceed 4GB (no RF64 support) */
    if(out_fmt==FMT_PERSEUS||out_fmt==FMT_JAGUAR){
        uint64_t target_sr = need_resamp ? (uint64_t)resamp_target : (uint64_t)rec.sample_rate;
        uint64_t max_data = 0xFFFFFFFFULL - 86;
        uint64_t expected_bytes = out_frames_actual*(uint64_t)out_ch*(uint64_t)(out_bps/8);
        if(expected_bytes > max_data){
            proc_log("Error: %s output would exceed 4 GB (%.1f GB).\n"
                     "  Maximum duration at this sample rate: %.0f seconds.\n"
                     "  Use Trim mode to select a shorter section.\n",
                     out_fmt==FMT_JAGUAR?"Jaguar":"Perseus",
                     (double)expected_bytes/1e9,
                     (double)max_data/(target_sr*4));
            seq_close(&sr); free(fir_h); return 1;
        }
    }

    /* Perseus<->Jaguar cross-conversion: only allowed at 2,000,000 Hz (both 16-bit).
     * At other rates the sample rates differ and conversion is not meaningful. */


    /* Warn if SDR Connect output would require RF64 */
    if(out_fmt==FMT_SDRCONNECT){
        uint64_t overhead=80;
        if(out_data_bytes >= 0xFFFFFFFFULL-overhead){
            proc_log(
                "Warning: SDR Connect output exceeds 4 GB and will be written as RF64.\n");
        }
    }

    int end_yy,end_mm2,end_dd,end_hh2,end_mi2,end_ss2;
    unix_to_utc(end_epoch,&end_yy,&end_mm2,&end_dd,&end_hh2,&end_mi2,&end_ss2);

    switch(out_fmt){
        case FMT_LINRAD:{
            LinradHdr oh=linrad_hdr;
            /* For non-Linrad input, populate fields from detected metadata */
            oh.sentinel=-1;
            oh.rx_input_mode=0x26;  /* required by WavViewDX for valid Linrad file */
            oh.timestamp=(double)start_epoch;
            proc_log("Linrad timestamp: %.0f (Unix epoch = %02d:%02d:%02d UTC)\n",
                     oh.timestamp,trim_sh,trim_sm,trim_ss);
            oh.passband_center=(double)ddc_cf_hz/1e6;
            oh.rx_rf_channels=(out_ch==4)?2:1;
            oh.rx_ad_channels=out_ch;
            oh.rx_ad_speed=fs_out;
            /* preserve passband_direction, rx_input_mode, save_init_flag from input
               (zeroed for non-Linrad input) */
            uint8_t hbuf[LINRAD_HDR_SIZE];
            linrad_to_bytes(&oh,hbuf);
            if(fwrite(hbuf,1,LINRAD_HDR_SIZE,fout)!=LINRAD_HDR_SIZE){
                proc_log("Error writing header.\n"); goto write_error; }
            break; }
        case FMT_WAVVIEWDX:
            /* Headerless  -  nothing to write */
            break;
        case FMT_SDRUNO:
        case FMT_SDRCONNECT:
        case FMT_PERSEUS:
        case FMT_JAGUAR: {
            /* hdr_sr: for DDC write the decimated rate; for passthrough write the input rate;
             * for resampled Jaguar output write the resampled rate. */
            uint32_t hdr_sr = need_resamp ? (uint32_t)resamp_target : (uint32_t)fs_out;
            /* Note: hdr_sr is already correct — resamp_target for resampled output,
             * fs_out (2MHz or 1.6MHz) for passthrough. No override needed. */
            data_sz32_pos=write_wav_header(fout,out_fmt,out_data_bytes,
                hdr_sr,(uint16_t)out_ch,(uint16_t)out_bps,(int32_t)ddc_cf_hz,
                out_year,out_month,out_day,trim_sh,trim_sm,trim_ss,
                end_yy,end_mm2,end_dd,end_hh2,end_mi2,end_ss2,
                outpath,&use_rf64,&ds64_pos);
            break; }
        default: break;
    }

    /* ── Process and stream data ── */
    int src_idx = (ochan==OUT_CH2)?2:(ochan==OUT_CH1)?0:-1;
    const char *label = do_ddc ? "DDC+Write " : "Writing   ";

    /* DDC filter warmup: seek back n_taps input frames before trim start
     * so the delay line is fully populated before output begins.
     * This eliminates the corrupted startup frames at the beginning of output. */
    uint64_t warmup_frames = 0;
    if(do_ddc && n_taps > 0){
        warmup_frames = (uint64_t)n_taps;
        if(warmup_frames > start_frame) warmup_frames = start_frame;
        if(warmup_frames > 0){
            uint64_t wstart = (start_frame >= warmup_frames) ? start_frame - warmup_frames : 0;
            seq_seek_frame(&sr, wstart, file_frames, sr.count);
            proc_log("DDC warmup  : %llu frames pre-read\n", (unsigned long long)warmup_frames);
        }
    }

    int rc;
    if(do_ddc){
        rc=copy_ddc(&sr,fout,warmup_frames,in_frames,rec.num_channels,out_ch,out_bps,
                    fir_h,n_taps,ddc_D,delta_phi,ochan,label);
    } else if(need_resamp){
        rc=copy_resample(&sr,fout,in_frames,rec.num_channels,out_ch,out_bps,
                         rec.sample_rate,resamp_target,ochan,label);
        fs_out=resamp_target;
    } else {
        rc=copy_passthrough(&sr,fout,in_frames,rec.num_channels,out_ch,out_bps,
                            src_idx,label);
    }
    if(rc<0) goto write_error;

    /* ── Finalise WAV headers ── */
    if(out_fmt==FMT_SDRUNO||out_fmt==FMT_SDRCONNECT||
       out_fmt==FMT_PERSEUS||out_fmt==FMT_JAGUAR){
        finalise_wav(fout,use_rf64,ds64_pos,data_sz32_pos,out_ch,out_bps);
    }

    seq_close(&sr); fclose(fout); free(fir_h);
    if(rc==2){
        proc_log("Cancelled. Partial output: %s\n",outpath);
        return 2;
    }
    proc_log("Done. Output: %s\n",outpath);
    return 0;

write_error:
    proc_log("Fatal error  -  removing partial output.\n");
    seq_close(&sr); fclose(fout); free(fir_h);
    remove(outpath);
    return 1;
}



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
#define CLR_RUN_BG      RGB(0,   158, 87 )   /* green  — idle, click to Run */
#define CLR_RUN_HOT     RGB(0,   134, 74 )
#define CLR_PAUSE_BG    RGB(217, 119, 6  )   /* amber  — running, click to Pause */
#define CLR_PAUSE_HOT   RGB(184, 101, 5  )
#define CLR_RESUME_BG   RGB(0,   120, 215)   /* blue   — paused, click to Resume */
#define CLR_RESUME_HOT  RGB(0,   102, 180)
#define CLR_RUN_TEXT    RGB(255, 255, 255)
#define CLR_CANCEL_BG       RGB(196, 43,  43 )   /* red — enabled */
#define CLR_CANCEL_HOT      RGB(165, 32,  32 )
#define CLR_CANCEL_DIS_BG   RGB(229, 229, 229)   /* grey — disabled */
#define CLR_CANCEL_DIS_TEXT RGB(160, 160, 160)
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
static int       g_sample_rate = 0;  /* sample rate of currently loaded file */
static int       g_fi_fmt      = -1; /* format of currently loaded file */
static wchar_t   g_out_folder[MAX_PATH] = {0}; /* custom output folder, or empty for same as input */
static wchar_t   g_rec_info[128] = {0};  /* recording start/end info string */
static int       g_dur_secs    = 0;   /* total recording duration in seconds */
static BOOL      g_outfile_warn = FALSE; /* TRUE when output label shows 4GB warning */
static wchar_t   g_sr_info[64]  = {0};  /* sample rate info string */

static HINSTANCE g_hinst;
static BOOL      g_running    = FALSE;
static BOOL      g_batch_run  = FALSE;
static HANDLE    g_thread     = NULL;
static HANDLE    g_hprocess   = NULL;
static volatile BOOL g_cancel = FALSE;  /* set to TRUE to abort processing thread */
static volatile BOOL g_pause  = FALSE;  /* set to TRUE to suspend processing thread */
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
{ _snwprintf(buf,n,L"%s\\sdrtrim.ini",g_exedir); }

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
static int  populate_bw_dropdown(HWND hwnd, int fs_in, int out_fmt);  /* forward declaration */
static BOOL sample_rate_decimates_cleanly(int fs_in);     /* forward declaration */
static void update_run_batch_button(void);   /* forward declaration */

static void save_settings(void)
{
    wchar_t b[MAX_PATH];
    ini_wi(L"S",L"Trim",  IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)?1:0);
    GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),b,10); ini_ws(L"S",L"Start",b);
    GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  b,10); ini_ws(L"S",L"End",  b);
    ini_wi(L"S",L"Chan",
        IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1)?1:
        IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2)?2:0);
    ini_wi(L"S",L"Fmt",
        IsDlgButtonChecked(g_hwnd,ID_FMT_LINRAD)    ?1:
        IsDlgButtonChecked(g_hwnd,ID_FMT_WAVVIEWDX) ?2:
        IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO)    ?3:
        IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT)?4:
        IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS)?5:
        IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR)?6:0);
    ini_wi(L"S",L"DDC", IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)?1:0);
    GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),b,16); ini_ws(L"S",L"DDCcf",b);
    ini_ws(L"S",L"OutFolder",g_out_folder);
    { HWND hcb=GetDlgItem(g_hwnd,ID_DDC_BW);
      int sel=(int)SendMessageW(hcb,CB_GETCURSEL,0,0);
      wchar_t bwlbl[32]={0};
      if(sel>=0) SendMessageW(hcb,CB_GETLBTEXT,sel,(LPARAM)bwlbl);
      ini_ws(L"S",L"DDCbw",bwlbl); }
}

static void restore_settings(void)
{
    wchar_t b[MAX_PATH];
    int trim=ini_ri(L"S",L"Trim",0);
    CheckRadioButton(g_hwnd,ID_MODE_FULL,ID_MODE_TRIM,trim?ID_MODE_TRIM:ID_MODE_FULL);
    EnableWindow(GetDlgItem(g_hwnd,ID_START_EDIT),trim);
    EnableWindow(GetDlgItem(g_hwnd,ID_END_EDIT),  trim);
    ini_rs(L"S",L"Start",L"",b,10); SetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),b);
    ini_rs(L"S",L"End",  L"",b,10); SetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  b);
    int chan=ini_ri(L"S",L"Chan",0);
    CheckRadioButton(g_hwnd,ID_CHAN_DUAL,ID_CHAN_CH2,
        chan==1?ID_CHAN_CH1:chan==2?ID_CHAN_CH2:ID_CHAN_DUAL);
    int fmt=ini_ri(L"S",L"Fmt",0);
    CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,
        fmt==1?ID_FMT_LINRAD:fmt==2?ID_FMT_WAVVIEWDX:
        fmt==3?ID_FMT_SDRUNO:fmt==4?ID_FMT_SDRCONNECT:
        fmt==5?ID_FMT_PERSEUS:fmt==6?ID_FMT_JAGUAR:ID_FMT_SAME);
    int ddc=ini_ri(L"S",L"DDC",0);
    CheckDlgButton(g_hwnd,ID_DDC_CHECK,ddc?BST_CHECKED:BST_UNCHECKED);
    EnableWindow(GetDlgItem(g_hwnd,ID_DDC_CENTRE),ddc);
    EnableWindow(GetDlgItem(g_hwnd,ID_DDC_BW),    ddc);
    ini_rs(L"S",L"DDCcf",L"",b,16); SetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),b);
    ini_rs(L"S",L"OutFolder",L"",g_out_folder,MAX_PATH);
    /* DDCbw is restored by populate_bw_dropdown when file is loaded */
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
    int  bits_per_sample;
    int  centre_hz;
    int  year,mon,day,hour,min,sec;
    int  utc_digit;
} FileInfo;

static uint32_t rd32(const uint8_t *b){ return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
static uint16_t rd16(const uint8_t *b){ return (uint16_t)b[0]|((uint16_t)b[1]<<8); }





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
        fi->bits_per_sample=16;
        u64=0; for(int i=0;i<8;i++) u64|=(uint64_t)hdr[4+i]<<(i*8);
        /* Linrad timestamp: new files store seconds-of-day, old files stored Unix epoch.
         * Distinguish by value: seconds-of-day is always < 86400. */
        /* timestamp is Unix epoch as double at bytes 4-11 */
        uint64_t ts_u64=0; for(int i=0;i<8;i++) ts_u64|=(uint64_t)hdr[4+i]<<(i*8);
        double ts; memcpy(&ts,&ts_u64,8);
        if(ts >= 86400.0){
            /* Unix epoch as double */
            int yy,mo,dd,hh,mi,ss;
            unix_to_utc((int64_t)ts,&yy,&mo,&dd,&hh,&mi,&ss);
            fi->hour=hh; fi->min=mi; fi->sec=ss;
        } else {
            /* Legacy seconds-of-day */
            int sod=(int)ts;
            fi->hour=sod/3600; fi->min=(sod%3600)/60; fi->sec=sod%60;
        }
        fi->year=0; fi->mon=0; fi->day=0;
        const wchar_t *bn=wcsrchr(path,L'\\'); if(!bn)bn=path; else bn++;
        /* Linrad filename: yyyymmdd_hhmmssU_xxxkHz.raw — the date/time block
         * may be preceded by a custom prefix (e.g. "RSPduo_dual_tuner_"), so
         * scan for the first occurrence of 8 digits + '_' + 6 digits rather
         * than assuming it starts at the beginning of the filename. */
        int yy=0,mo=0,dd=0,hh=0,mi=0,ss=0;
        for(const wchar_t *p=bn; *p; p++){
            if(swscanf(p,L"%4d%2d%2d_%2d%2d%2d",&yy,&mo,&dd,&hh,&mi,&ss)==6
               && yy>2000 && mo>=1 && mo<=12 && dd>=1 && dd<=31
               && hh>=0 && hh<24 && mi>=0 && mi<60 && ss>=0 && ss<60){
                fi->year=yy; fi->mon=mo; fi->day=dd;
                fi->hour=hh; fi->min=mi; fi->sec=ss;
                bn=p; /* anchor utc_digit lookup to the matched date block */
                break;
            }
        }
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
                /* PCM fmt: channels at +2, sample_rate at +4, bits at +14 (all relative to chunk data start) */
                if(pos+8+10<=(uint32_t)got){
                    fi->sample_rate=(int)rd32(hdr+pos+12);  /* pos+8+4 */
                    uint16_t nch=rd16(hdr+pos+10);           /* pos+8+2 */
                    fi->dual=(nch==4)?1:0;
                    if(pos+8+16<=(uint32_t)got)
                        fi->bits_per_sample=(int)rd16(hdr+pos+22); /* pos+8+14 */
                    else
                        fi->bits_per_sample=16;
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
                    if(swscanf(stem,L"SDRuno_%4d%2d%2d_%2d%2d%2dZ_%dkHz",
                        &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&cf_khz)<7)
                        swscanf(stem,L"SDRuno_%4d%2d%2d_%2d%2d%2d_%dkHz",
                        &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&cf_khz);
                    if(cf_khz>0&&fi->centre_hz==0) fi->centre_hz=cf_khz*1000;
                }
                return TRUE;
            }
            if(memcmp(id,"rcvr",4)==0 && have_fmt){
                /* Perseus (flags=4) or Jaguar (flags=5) */
                uint32_t ad=pos+8;
                if(ad+12<=(uint32_t)got){
                    fi->centre_hz=(int)rd32(hdr+ad+0);
                    uint32_t flags=rd32(hdr+ad+4);
                    uint32_t ts  =rd32(hdr+ad+8);
                    /* flags=5 -> Jaguar 1.6MHz; flags=4 -> check padding byte 16 */
                    if(flags==5)
                        fi->fmt=5;
                    else if(flags==4 && hdr[ad+16]==0x00)
                        fi->fmt=5;  /* Jaguar 2MHz */
                    else
                        fi->fmt=4;  /* Perseus */
                    int64_t ep=(int64_t)ts;
                    unix_to_utc(ep,&fi->year,&fi->mon,&fi->day,
                                   &fi->hour,&fi->min,&fi->sec);
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
            if(swscanf(stem,L"SDRuno_%4d%2d%2d_%2d%2d%2dZ_%dkHz",
                &fi->year,&fi->mon,&fi->day,&fi->hour,&fi->min,&fi->sec,&cf_khz)<7)
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
            fi->fmt=1; fi->bits_per_sample=16; int ch=0; swscanf(pat+11,L"%d",&ch); fi->dual=(ch==2)?1:0;
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
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS))    out_fmt=4;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR))     out_fmt=5;
    else out_fmt=fi.fmt;
    if(out_fmt<0) out_fmt=0;
    int dual_out=fi.dual;
    if(IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1)||IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2)) dual_out=0;
    int cf=fi.centre_hz;
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t buf[16]; GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),buf,16);
        int ddc_cf=_wtoi(buf); if(ddc_cf>0) cf=ddc_cf*1000;
    }
    int sy=fi.year,smo=fi.mon,sd=fi.day,sh=fi.hour,smi=fi.min,ss=fi.sec;
    if(IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)){
        wchar_t buf[8]; int hh=0,mm=0,hh_s=0;
        GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),buf,8);
        if(wcslen(buf)==6) swscanf(buf,L"%2d%2d%2d",&hh,&mm,&hh_s);
        else if(wcslen(buf)==4) swscanf(buf,L"%2d%2d",&hh,&mm);
        int64_t file_ep=utc_to_unix(fi.year,fi.mon,fi.day,fi.hour,fi.min,fi.sec);
        int64_t start_ep=utc_to_unix(fi.year,fi.mon,fi.day,hh,mm,hh_s);
        if(start_ep<file_ep){
            int64_t next=start_ep+86400; int dummy_s;
            unix_to_utc(next,&sy,&smo,&sd,&sh,&smi,&dummy_s);
            smi=mm; sh=hh; ss=hh_s;
        } else { sy=fi.year; smo=fi.mon; sd=fi.day; sh=hh; smi=mm; ss=hh_s; }
    }
    int sr=fi.sample_rate;
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        HWND hcb=GetDlgItem(g_hwnd,ID_DDC_BW);
        int sel=(int)SendMessageW(hcb,CB_GETCURSEL,0,0);
        if(sel>=0){
            int D_val=(int)SendMessageW(hcb,CB_GETITEMDATA,sel,0);
            if(D_val>0 && sr>0) sr=sr/D_val;
        }
    }
    wchar_t name[MAX_PATH];

    switch(out_fmt){
    case 0: _snwprintf(name,MAX_PATH,L"%04d%02d%02d_%02d%02d%02dZ_%dkHz.raw",
                sy,smo,sd,sh,smi,ss,cf/1000); break;
    case 1: _snwprintf(name,MAX_PATH,L"iq_pcm16_ch%d_cf%d_sr%d_dt%04d%02d%02d-%02d%02d%02d.raw",
                dual_out?2:1,cf,sr,sy,smo,sd,sh,smi,ss); break;
    case 2: _snwprintf(name,MAX_PATH,L"SDRuno_%04d%02d%02d_%02d%02d%02dZ_%dkHz.wav",
                sy,smo,sd,sh,smi,ss,cf/1000); break;
    case 3: _snwprintf(name,MAX_PATH,L"SDRconnect_IQ_%04d%02d%02d_%02d%02d%02d_%dHZ.wav",
                sy,smo,sd,sh,smi,ss,cf); break;
    case 4: _snwprintf(name,MAX_PATH,L"Perseus_%04d%02d%02d_%02d%02d%02dZ_%dkHz.wav",
                sy,smo,sd,sh,smi,ss,cf/1000); break;
    case 5: _snwprintf(name,MAX_PATH,L"Jaguar_%04d%02d%02d_%02d%02d%02dZ_%dkHz.wav",
                sy,smo,sd,sh,smi,ss,cf/1000); break;
    default: return FALSE;
    }
    wchar_t dir[MAX_PATH];
    if(g_out_folder[0]){
        /* Use custom output folder */
        wcsncpy(dir,g_out_folder,MAX_PATH-1); dir[MAX_PATH-1]=L'\0';
        /* Ensure trailing backslash */
        size_t dl=wcslen(dir);
        if(dl>0 && dir[dl-1]!=L'\\') { dir[dl]=L'\\'; dir[dl+1]=L'\0'; }
    } else {
        /* Default: same folder as input */
        wcsncpy(dir,inpath,MAX_PATH-1);
        wchar_t *sl=wcsrchr(dir,L'\\'); if(sl){ sl[1]=L'\0'; } else { dir[0]=L'\0'; }
    }
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
    /* argv[0] is a placeholder — sdr_process_args ignores it */
    _snwprintf(cmd,n,L"sdrtrimgui \"%s\"",input);
    if(IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)){
        wchar_t s[10],e[10];
        GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),s,10);
        GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  e,10);
        if((wcslen(s)!=4&&wcslen(s)!=6)||(wcslen(e)!=4&&wcslen(e)!=6)){
            MessageBoxW(g_hwnd,L"Start and end times must be HHMM or HHMMSS (e.g. 030000).",
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
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS))    wcsncat(cmd,L" perseus",   n-wcslen(cmd)-1);
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR))     wcsncat(cmd,L" jaguar",    n-wcslen(cmd)-1);
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t cf[16],bw[16];
        GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),cf,16);
        { HWND hcb=GetDlgItem(g_hwnd,ID_DDC_BW);
          int sel=(int)SendMessageW(hcb,CB_GETCURSEL,0,0);
          if(sel>=0){
              /* item data = D; recover bw_khz from D and sample rate */
              int D_val=(int)SendMessageW(hcb,CB_GETITEMDATA,sel,0);
              if(D_val>0 && g_sample_rate>0){
                  int fs_out_val=g_sample_rate/D_val;
                  swprintf(bw,16,L"%d",fs_out_val/1000);
              }
          } }
        if(!cf[0]||!bw[0]){
            MessageBoxW(g_hwnd,
                bw[0] ? L"DDC is enabled but no centre frequency is set."
                       : L"DDC is enabled but no bandwidth is selected.",
                L"SDR Trim",MB_OK|MB_ICONWARNING);
            return FALSE;
        }
        wchar_t d[64]; _snwprintf(d,64,L" --ddc %s %s",cf,bw);
        wcsncat(cmd,d,n-wcslen(cmd)-1);
    }
    /* Append --24bit flag for Perseus output if 24-bit is selected */
    if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS) &&
       IsDlgButtonChecked(g_hwnd,ID_OUT_24BIT)){
        wcsncat(cmd,L" --24bit",n-wcslen(cmd)-1);
    } else if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS) &&
              IsDlgButtonChecked(g_hwnd,ID_OUT_16BIT)){
        wcsncat(cmd,L" --16bit",n-wcslen(cmd)-1);
    }
    /* Append --outdir if a custom output folder is set */
    if(g_out_folder[0]){
        wcsncat(cmd,L" --outdir \"",n-wcslen(cmd)-1);
        wcsncat(cmd,g_out_folder,n-wcslen(cmd)-1);
        wcsncat(cmd,L"\"",n-wcslen(cmd)-1);
    }
    /* Append --seq paths for each sequence file */
    for(int si=0;si<g_seq_count;si++){
        wcsncat(cmd,L" --seq \"",n-wcslen(cmd)-1);
        wcsncat(cmd,g_seq_paths[si],n-wcslen(cmd)-1);
        wcsncat(cmd,L"\"",n-wcslen(cmd)-1);
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
        wchar_t s[10]={0},e[10]={0};
        GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),s,10);
        GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),  e,10);
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
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS))    fmt=L" \u2192 perseus";
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR))     fmt=L" \u2192 jaguar";
    wcsncat(parts,fmt,128-wcslen(parts)-1);
    /* DDC */
    if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
        wchar_t cf[16]={0},bw[16]={0};
        GetWindowTextW(GetDlgItem(g_hwnd,ID_DDC_CENTRE),cf,16);
        { HWND hcb=GetDlgItem(g_hwnd,ID_DDC_BW);
          int sel=(int)SendMessageW(hcb,CB_GETCURSEL,0,0);
          if(sel>=0){
              /* item data = D; recover bw_khz from D and sample rate */
              int D_val=(int)SendMessageW(hcb,CB_GETITEMDATA,sel,0);
              if(D_val>0 && g_sample_rate>0){
                  int fs_out_val=g_sample_rate/D_val;
                  swprintf(bw,16,L"%d",fs_out_val/1000);
              }
          } }
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
    g_sample_rate = fi.sample_rate;
    g_fi_fmt      = fi.fmt;

    /* Build recording time info string */
    if(input[0] && fi.fmt >= 0 && fi.sample_rate > 0){
        struct _stat64 st; int dur_sec=0;
        if(_wstat64(input,&st)==0 && st.st_size > 0){
            int ch = fi.dual ? 4 : 2;
            int bps = (fi.bits_per_sample > 0) ? (fi.bits_per_sample/8) : 2;
            int64_t data_bytes = st.st_size - (fi.fmt<=1 ? 41 : 200);
            if(data_bytes > 0)
                dur_sec = (int)(data_bytes / ((int64_t)fi.sample_rate * ch * bps));
        }
        /* Add durations of any sequence files */
        for(int si=0; si<g_seq_count; si++){
            struct _stat64 st2;
            if(_wstat64(g_seq_paths[si],&st2)==0 && st2.st_size > 0){
                int ch = fi.dual ? 4 : 2;
                int64_t data_bytes = st2.st_size - 200; /* WAV header allowance */
                if(data_bytes > 0)
                    dur_sec += (int)(data_bytes / ((int64_t)fi.sample_rate * ch * 2));
            }
        }
        int end_sod = fi.hour*3600 + fi.min*60 + fi.sec + dur_sec;
        _snwprintf(g_rec_info,128,
            L"Recording:  %02d:%02d:%02d — %02d:%02d:%02d UTC  (%dh %02dm)",
            fi.hour,fi.min,fi.sec,
            (end_sod/3600)%24,(end_sod%3600)/60,end_sod%60,
            dur_sec/3600,(dur_sec%3600)/60);
        g_dur_secs = dur_sec;
    } else {
        wcscpy(g_rec_info,L"Recording:  (no file loaded)");
        g_dur_secs = 0;
    }
        SetWindowTextW(GetDlgItem(g_hwnd,ID_REC_INFO),g_rec_info);

    /* Sample rate display */
    if(input[0] && fi.sample_rate > 0){
        int bps_disp = fi.bits_per_sample > 0 ? fi.bits_per_sample : 16;
        _snwprintf(g_sr_info,64,L"Sample rate: %d sps  %d-bit",fi.sample_rate,bps_disp);
    } else {
        g_sr_info[0]=L'\0';
    }
    SetWindowTextW(GetDlgItem(g_hwnd,ID_SR_INFO),g_sr_info);

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



/* Show/hide the bit-depth radio buttons (only visible for Perseus output).
 * set_default=TRUE resets to the natural bit depth for the rate. */
static void update_24bit_control(int out_sample_rate, int in_bps, BOOL perseus_out, BOOL set_default)
{

    /* 2,000,000 Hz and 1,999,000 Hz Perseus output must be 16-bit — force it
     * and hide the choice since there is no valid alternative. */
    BOOL force16 = (out_sample_rate == 2000000 || out_sample_rate == 1999000);
    BOOL show_choice = perseus_out && !force16;

    ShowWindow(GetDlgItem(g_hwnd, ID_BIT_LABEL), show_choice ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hwnd, ID_OUT_16BIT), show_choice ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(g_hwnd, ID_OUT_24BIT), show_choice ? SW_SHOW : SW_HIDE);

    if(!perseus_out || force16){
        CheckRadioButton(g_hwnd, ID_OUT_16BIT, ID_OUT_24BIT, ID_OUT_16BIT);
        return;
    }
    if(set_default){
        /* Default to match the input bit depth — 24-bit if input is 24-bit,
         * 16-bit otherwise (e.g. converting from Linrad/SDRuno). */
        BOOL def24 = (in_bps == 24);
        CheckRadioButton(g_hwnd, ID_OUT_16BIT, ID_OUT_24BIT,
                         def24 ? ID_OUT_24BIT : ID_OUT_16BIT);
    }
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
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_PERSEUS),    TRUE);
    EnableWindow(GetDlgItem(g_hwnd,ID_FMT_JAGUAR),     TRUE);

    if(!input[0]) return;

    FileInfo fi; fi.fmt=-1; fi.dual=0;
    probe_file(input,&fi);
    if(fi.fmt<0) return;


    if(fi.fmt!=4 && fi.fmt!=5){
        /* Non-Perseus/Jaguar input: Perseus output requires resampling to a
         * Perseus-native rate — only allow if input rate matches a Perseus rate */
        static const int perseus_rates[]={125000,250000,500000,1000000,2000000,1999000,0};
        BOOL ok_per=FALSE;
        for(int i=0;perseus_rates[i];i++) if(fi.sample_rate==perseus_rates[i]){ok_per=TRUE;break;}
        if(!ok_per){
            if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS))
                CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_SAME);
            EnableWindow(GetDlgItem(g_hwnd,ID_FMT_PERSEUS), FALSE);
        }
        /* Jaguar output: available at any sample rate (resampler converts to 1,600,000 Hz)
         * except when DDC is active at a non-Jaguar rate */
        BOOL ok_jag = !IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK); /* Jaguar available for passthrough at any rate */
        if(!ok_jag){
            if(IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR))
                CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_SAME);
            EnableWindow(GetDlgItem(g_hwnd,ID_FMT_JAGUAR), FALSE);
        }
    }

    BOOL trimming   = IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM);
    BOOL ddc        = IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK);

    /* Grey out SDRuno/SDRConnect if effective output rate != 2MHz.
     * When DDC active use the decimated rate, not the input rate. */
    int eff_out_rate = fi.sample_rate;
    if(ddc && fi.sample_rate > 0){
        HWND hcb_sr = GetDlgItem(g_hwnd, ID_DDC_BW);
        int sel_sr = (int)SendMessageW(hcb_sr, CB_GETCURSEL, 0, 0);
        if(sel_sr >= 0){
            int D_sr = (int)SendMessageW(hcb_sr, CB_GETITEMDATA, (WPARAM)sel_sr, 0);
            if(D_sr > 0) eff_out_rate = fi.sample_rate / D_sr;
        }
    }
    if(eff_out_rate > 0 && eff_out_rate != 2000000){
        /* SDRuno hardcodes 2MHz — grey it out for non-2MHz output.
         * SDR Connect reads the fmt chunk so it handles variable rates. */
        if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO))
            CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_SAME);
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SDRUNO), FALSE);
    }
    /* ch1/ch2 only counts as extraction when input is dual-channel */
    BOOL ch_extract = fi.dual &&
                      (IsDlgButtonChecked(g_hwnd,ID_CHAN_CH1) ||
                       IsDlgButtonChecked(g_hwnd,ID_CHAN_CH2));
    BOOL dual_out   = fi.dual && !ch_extract;

    /* Perseus/Jaguar cross-format: only allow at 2,000,000 Hz (both 16-bit). */
    if(fi.fmt==4){  /* Perseus input */
        BOOL allow_jag = (fi.sample_rate == 2000000);
        if(!allow_jag){
            if(IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR))
                CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_PERSEUS);
        }
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_JAGUAR), allow_jag);
    } else if(fi.fmt==5){  /* Jaguar input */
        BOOL allow_per = (fi.sample_rate == 2000000);
        if(!allow_per){
            if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS))
                CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_JAGUAR);
        }
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_PERSEUS), allow_per);
    }

    /* Compute DDC output rate from selected BW dropdown */
    int ddc_out_rate = 0;
    if(ddc){
        HWND hcb2 = GetDlgItem(g_hwnd, ID_DDC_BW);
        int sel2 = (int)SendMessageW(hcb2, CB_GETCURSEL, 0, 0);
        if(sel2 >= 0){
            int D2 = (int)SendMessageW(hcb2, CB_GETITEMDATA, (WPARAM)sel2, 0);
            if(D2 > 0) ddc_out_rate = fi.sample_rate / D2;
        }
    }
    /* Disable Perseus/Jaguar output for non-native DDC output rates */
    { static const int nat[]={125000,250000,500000,1000000,2000000,0};
      BOOL native_ddc = !ddc;
      for(int ni=0; nat[ni] && !native_ddc; ni++)
          if(ddc_out_rate==nat[ni]) native_ddc=TRUE;
      if(!native_ddc){
          if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS)||
             IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR))
              CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_SAME);
          EnableWindow(GetDlgItem(g_hwnd,ID_FMT_PERSEUS), FALSE);
          EnableWindow(GetDlgItem(g_hwnd,ID_FMT_JAGUAR),  FALSE);
      }
    }
    /* DDC availability: rates with only 2/5 prime factors give clean output.
     * 1,999,000 Hz has prime factor 1999 â allow DDC but warn of ~0.05% error. */
    /* Resolve effective output format for BW filter */
    int eff_fmt_bw = fi.fmt;  /* default: same as input */
    if(IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS))     eff_fmt_bw=4;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR))  eff_fmt_bw=5;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_LINRAD))  eff_fmt_bw=0;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_WAVVIEWDX)) eff_fmt_bw=1;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO))  eff_fmt_bw=2;
    else if(IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT)) eff_fmt_bw=3;
    int n_ddc_opts = populate_bw_dropdown(g_hwnd, fi.sample_rate, eff_fmt_bw);
    HWND hu = GetDlgItem(g_hwnd, ID_DDC_UNAVAIL);
    if(n_ddc_opts == 0 && fi.sample_rate == 1999000){
        EnableWindow(GetDlgItem(g_hwnd,ID_DDC_CHECK),TRUE);
        HWND hcb=GetDlgItem(g_hwnd,ID_DDC_BW);
        SendMessageW(hcb,CB_RESETCONTENT,0,0);
        int sr=fi.sample_rate;
        for(int D=2;D<=sr/50000;D++){
            if(sr%D!=0) continue;
            int fo=sr/D; if(fo<50000) break;
            wchar_t lbl[32];
            if(fo%1000==0) swprintf(lbl,32,L"%d kHz",fo/1000);
            else swprintf(lbl,32,L"%.1f kHz",(double)fo/1000.0);
            int idx=(int)SendMessageW(hcb,CB_ADDSTRING,0,(LPARAM)lbl);
            SendMessageW(hcb,CB_SETITEMDATA,(WPARAM)idx,(LPARAM)D);
        }
        SendMessageW(hcb,CB_SETCURSEL,0,0);
        SetWindowTextW(hu,L"⚠ Frequency accuracy ±0.05%");
        ShowWindow(hu,SW_SHOW);
    } else if(n_ddc_opts == 0){
        if(IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
            CheckDlgButton(g_hwnd,ID_DDC_CHECK,BST_UNCHECKED);
            EnableWindow(GetDlgItem(g_hwnd,ID_DDC_CENTRE),FALSE);
            EnableWindow(GetDlgItem(g_hwnd,ID_DDC_BW),    FALSE);
        }
        EnableWindow(GetDlgItem(g_hwnd,ID_DDC_CHECK),FALSE);
        SetWindowTextW(hu,L"Not available at this sample rate");
        ShowWindow(hu,SW_SHOW);
    } else {
        EnableWindow(GetDlgItem(g_hwnd,ID_DDC_CHECK),TRUE);
        ShowWindow(hu,SW_HIDE);
    }

    /* For dual output, sdruno and sdrconnect are never valid (single-tuner only) */
    if(dual_out){
        /* Disable WAV formats — they can't hold dual-tuner data */
        BOOL wav_sel = IsDlgButtonChecked(g_hwnd,ID_FMT_SDRUNO) ||
                       IsDlgButtonChecked(g_hwnd,ID_FMT_SDRCONNECT) ||
                       IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS) ||
                       IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR);
        if(wav_sel)
            CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,
                fi.fmt==0?ID_FMT_LINRAD:ID_FMT_WAVVIEWDX);
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SDRUNO),     FALSE);
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SDRCONNECT), FALSE);
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_PERSEUS),    FALSE);
        EnableWindow(GetDlgItem(g_hwnd,ID_FMT_JAGUAR),     FALSE);
        /* "Same as input" for dual linrad/wavviewdx is only a no-op if no trim/ddc */
        if(!trimming && !ddc){
            int noop_id = (fi.fmt==0)?ID_FMT_LINRAD:
                          (fi.fmt==1)?ID_FMT_WAVVIEWDX:-1;
            if(noop_id>=0){
                if(IsDlgButtonChecked(g_hwnd,ID_FMT_SAME)||
                   IsDlgButtonChecked(g_hwnd,noop_id)){
                    int other = (noop_id==ID_FMT_LINRAD)?ID_FMT_WAVVIEWDX:ID_FMT_LINRAD;
                    CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,other);
                }
                EnableWindow(GetDlgItem(g_hwnd,noop_id),    FALSE);
                EnableWindow(GetDlgItem(g_hwnd,ID_FMT_SAME),FALSE);
            }
        }
        return;
    }

    /* Update bit-depth radio visibility — only for Perseus output */
    {
        /* Effective output format — resolve "Same as input" */
        BOOL per_out = IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS) ||
                       (IsDlgButtonChecked(g_hwnd,ID_FMT_SAME) && fi.fmt==4);
        /* Effective output sample rate — use DDC rate if DDC active */
        int bdc_out_sr = fi.sample_rate;
        if(per_out && IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK)){
            HWND hcb_bd = GetDlgItem(g_hwnd, ID_DDC_BW);
            int sel_bd = (int)SendMessageW(hcb_bd, CB_GETCURSEL, 0, 0);
            if(sel_bd >= 0){
                int D_bd = (int)SendMessageW(hcb_bd, CB_GETITEMDATA, (WPARAM)sel_bd, 0);
                if(D_bd > 0) bdc_out_sr = fi.sample_rate / D_bd;
            }
        }
        update_24bit_control(bdc_out_sr, fi.bits_per_sample, per_out, per_out);
    }

    /* Single output: disable same-format if it would be a no-op */
    if(trimming || ddc) return;

    /* Ensure something is always selected. If the currently selected button
     * is now disabled (greyed out by rate rules above), fall back to Same as input. */
    BOOL any_checked = FALSE;
    int fmt_ids[]={ID_FMT_SAME,ID_FMT_LINRAD,ID_FMT_WAVVIEWDX,
                   ID_FMT_SDRUNO,ID_FMT_SDRCONNECT,ID_FMT_PERSEUS,ID_FMT_JAGUAR};
    for(int i=0;i<7;i++){
        if(IsDlgButtonChecked(g_hwnd,fmt_ids[i]) &&
           IsWindowEnabled(GetDlgItem(g_hwnd,fmt_ids[i]))){
            any_checked=TRUE; break;
        }
    }
    if(!any_checked)
        CheckRadioButton(g_hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_SAME);
}


static void update_prediction(void)
{
    wchar_t input[MAX_PATH];
    GetWindowTextW(GetDlgItem(g_hwnd,ID_INPUT_EDIT),input,MAX_PATH);
    if(!input[0]){
        SetWindowTextW(GetDlgItem(g_hwnd,ID_OUTFILE_STATIC),L"Output: (select input file)");
        return;
    }
    g_outfile_warn = FALSE;
    wchar_t outfile[MAX_PATH];
    if(predict_outfile(input,outfile,MAX_PATH)){
        wchar_t label[MAX_PATH+64];
        wchar_t *fn=wcsrchr(outfile,L'\\');
        _snwprintf(label,MAX_PATH+64,L"Output: %s",fn?fn+1:outfile);

        /* Warn if Perseus or Jaguar output would exceed 4 GB */
        BOOL per_jag = IsDlgButtonChecked(g_hwnd,ID_FMT_PERSEUS) ||
                       IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR)  ||
                       (IsDlgButtonChecked(g_hwnd,ID_FMT_SAME) &&
                        (g_fi_fmt==4||g_fi_fmt==5));
        if(per_jag && g_sample_rate > 0 && g_dur_secs > 0){
            /* Effective duration: trim mode narrows it */
            int eff_secs = g_dur_secs;
            if(IsDlgButtonChecked(g_hwnd,ID_MODE_TRIM)){
                wchar_t ss[16],es[16];
                GetWindowTextW(GetDlgItem(g_hwnd,ID_START_EDIT),ss,16);
                GetWindowTextW(GetDlgItem(g_hwnd,ID_END_EDIT),es,16);
                int sh=0,sm=0,ssc=0,eh=0,em=0,esc=0;
                swscanf(ss,L"%2d%2d%2d",&sh,&sm,&ssc);
                swscanf(es,L"%2d%2d%2d",&eh,&em,&esc);
                int trim_secs=(eh*3600+em*60+esc)-(sh*3600+sm*60+ssc);
                if(trim_secs>0 && trim_secs<eff_secs) eff_secs=trim_secs;
            }
            /* out_ch: DDC always produces single channel */
            int out_ch = (!IsDlgButtonChecked(g_hwnd,ID_DDC_CHECK) &&
                           IsDlgButtonChecked(g_hwnd,ID_CHAN_DUAL)) ? 4 : 2;
            int out_bps = IsDlgButtonChecked(g_hwnd,ID_OUT_24BIT) ? 24 : 16;
            /* For Jaguar output use the output rate (1,600,000 or 2,000,000) not input rate */
            int est_sr = g_sample_rate;
            if(IsDlgButtonChecked(g_hwnd,ID_FMT_JAGUAR) &&
               g_sample_rate != 1600000 && g_sample_rate != 2000000)
                est_sr = 1600000;
            uint64_t est_bytes = (uint64_t)eff_secs * (uint64_t)est_sr
                                 * (uint64_t)out_ch * (uint64_t)(out_bps/8);
            uint64_t max_bytes = 0xFFFFFFFFULL - 128;
            if(est_bytes > max_bytes){
                double max_secs = (double)max_bytes /
                                  ((double)est_sr * out_ch * (out_bps/8));
g_outfile_warn = TRUE;
                wcscat(label, L"  ⚠ Exceeds 4 GB limit");
                wchar_t warn[64];
                _snwprintf(warn,64,L" — max %.0f s at this rate",max_secs);
                wcscat(label, warn);
            }
        }
        SetWindowTextW(GetDlgItem(g_hwnd,ID_OUTFILE_STATIC),label);
        InvalidateRect(GetDlgItem(g_hwnd,ID_OUTFILE_STATIC),NULL,TRUE);
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
    /* Parse cmdline into argv[] and call sdr_process_args() directly */
    /* Format: "path\\sdrtrim.exe" "infile" [options...] */
    /* Skip argv[0] (exe path) — sdr_process_args expects argv[1]=infile */
    wchar_t tmp[4096];
    wcsncpy(tmp, cmdline, 4095); tmp[4095]=L'\0';

    /* Tokenise respecting quoted strings */
    char *argv_buf[64];
    int argc = 0;
    static char arg_store[64][512];
    wchar_t *p = tmp;
    while(*p && argc < 63){
        while(*p==L' ') p++;
        if(!*p) break;
        wchar_t *start;
        int quoted = (*p==L'"');
        if(quoted){ p++; start=p; while(*p&&*p!=L'"') p++; }
        else       { start=p;    while(*p&&*p!=L' ') p++; }
        int len=(int)(p-start);
        WideCharToMultiByte(CP_UTF8,0,start,len,arg_store[argc],511,NULL,NULL);
        arg_store[argc][len]='\0';
        argv_buf[argc]=arg_store[argc];
        argc++;
        if(quoted&&*p==L'"') p++;
    }
    argv_buf[argc]=NULL;

    if(argc < 2) return 1;  /* need at least exe + infile */

    g_proc_hwnd = g_hwnd;
    int rc = sdr_process_args(argc, argv_buf);
    return (DWORD)rc;
}

/* ------------------------------------------------------------------ */
/*  Single-job worker thread                                          */
/* ------------------------------------------------------------------ */
typedef struct { wchar_t cmdline[4096]; } ThreadArgs;

static DWORD WINAPI run_thread(LPVOID param)
{
    g_cancel=FALSE;
    ThreadArgs *args=(ThreadArgs*)param;
    DWORD ec=run_one_job(args->cmdline);
    wchar_t done[128];
    if(ec==(DWORD)-1)
        _snwprintf(done,128,L"Processing failed.\r\n");
    else if(ec==0)
        _snwprintf(done,128,L"Completed successfully.\r\n");
    else if(ec==2)
        _snwprintf(done,128,L"Cancelled.\r\n");
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
        if(!g_batch_run){
            /* Batch was cancelled before this job started */
            PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(L"Cancelled"));
            continue;
        }
        g_cancel=FALSE;
        /* Skip if already cancelled */
        if(wcscmp(g_jobs[i].status,L"Cancelled")==0) continue;

        /* Mark running */
        PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(L"Running"));

        /* Log header */
        wchar_t hdr[4200];
        _snwprintf(hdr,4200,L"--- Job %d of %d: %s ---\r\n> %s\r\n",
            i+1,g_job_count,g_jobs[i].input_short,g_jobs[i].cmdline);
        PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(hdr));

        /* Reset progress */
        PostMessageW(g_hwnd,WM_UPDATEPROG,0,0);

        DWORD ec=run_one_job(g_jobs[i].cmdline);

        if(ec==2){
            /* Job itself was cancelled mid-copy */
            PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(L"Cancelled"));
            /* Cancel any remaining pending jobs too */
            for(int j=i+1;j<g_job_count;j++)
                if(wcscmp(g_jobs[j].status,L"Pending")==0)
                    PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)j,(LPARAM)_wcsdup(L"Cancelled"));
            wchar_t done2[64];
            _snwprintf(done2,64,L"Job %d cancelled.\r\n\r\n",i+1);
            PostMessageW(g_hwnd,WM_APPENDLOG,0,(LPARAM)_wcsdup(done2));
            break;
        } else if(ec==(DWORD)-1 || ec!=0){
            const wchar_t *st=L"Failed";
            if(!g_batch_run){
                PostMessageW(g_hwnd,WM_BATCHSTATUS,(WPARAM)i,(LPARAM)_wcsdup(L"Cancelled"));
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
    COLORREF bg_normal, bg_hot;
    const wchar_t *label;
    if(!g_running)      { bg_normal=CLR_RUN_BG;    bg_hot=CLR_RUN_HOT;    label=L"Run";    }
    else if(g_pause)    { bg_normal=CLR_RESUME_BG; bg_hot=CLR_RESUME_HOT; label=L"Resume"; }
    else                { bg_normal=CLR_PAUSE_BG;  bg_hot=CLR_PAUSE_HOT;  label=L"Pause";  }
    COLORREF bg=pressed?bg_hot:bg_normal;
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
    DrawTextW(di->hDC,label,-1,&di->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    if(di->itemState&ODS_FOCUS){ RECT fr=di->rcItem; InflateRect(&fr,-3,-3); DrawFocusRect(di->hDC,&fr); }
}

static void draw_cancel_button(DRAWITEMSTRUCT *di)
{
    BOOL pressed  = (di->itemState&ODS_SELECTED)!=0;
    BOOL disabled = (di->itemState&ODS_DISABLED)!=0;
    COLORREF bg, txt;
    if(disabled){ bg=CLR_CANCEL_DIS_BG; txt=CLR_CANCEL_DIS_TEXT; }
    else        { bg=pressed?CLR_CANCEL_HOT:CLR_CANCEL_BG; txt=CLR_RUN_TEXT; }
    HBRUSH hbr=CreateSolidBrush(bg);
    FillRect(di->hDC,&di->rcItem,hbr); DeleteObject(hbr);
    HPEN hp=CreatePen(PS_SOLID,1,disabled?RGB(180,180,180):RGB(120,24,24));
    HPEN op=(HPEN)SelectObject(di->hDC,hp);
    HBRUSH ob=(HBRUSH)SelectObject(di->hDC,GetStockObject(NULL_BRUSH));
    RECT r=di->rcItem; r.right--; r.bottom--;
    Rectangle(di->hDC,r.left,r.top,r.right,r.bottom);
    SelectObject(di->hDC,op); SelectObject(di->hDC,ob); DeleteObject(hp);
    SetBkMode(di->hDC,TRANSPARENT);
    SetTextColor(di->hDC,txt);
    if(g_font_bold) SelectObject(di->hDC,g_font_bold);
    DrawTextW(di->hDC,L"Cancel",-1,&di->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
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
/* Populate the DDC bandwidth combobox based on input file sample rate.
 * Shows only rates where fs_in % D == 0 AND fs_out >= 50000 AND fs_out % 100 == 0.
 * Stores D as item data so run_job can recover it without string parsing. */
/* True if fs_in's only prime factors are 2 and 5. Sample rates built purely
 * from 2s and 5s decimate down to genuinely round output rates. A rate that
 * carries any other prime factor (e.g. 1,999,000 = 2^3*5^3*1999) can never
 * produce a truly clean decimated rate no matter what D is chosen, even if a
 * given D happens to leave fs_out divisible by 100 — confirmed by testing:
 * 1,999,000 Hz Perseus DDC'd output measured 0.05% off frequency in HDSDR. */
static BOOL sample_rate_decimates_cleanly(int fs_in)
{
    if(fs_in <= 0) return FALSE;
    while(fs_in % 2 == 0) fs_in /= 2;
    while(fs_in % 5 == 0) fs_in /= 5;
    return fs_in == 1;
}

static int populate_bw_dropdown(HWND hwnd, int fs_in, int out_fmt)
{
    /* out_fmt==4 (Perseus): only show rates native to Perseus hardware so
     * WavViewDX can play the output correctly (125k/250k/500k/1000k). */
    BOOL perseus_out = (out_fmt==4);
    static const int perseus_native[]={125000,250000,500000,1000000,0};

    HWND hcb = GetDlgItem(hwnd, ID_DDC_BW);
    SendMessageW(hcb, CB_RESETCONTENT, 0, 0);
    if(fs_in <= 0) return 0;
    if(!sample_rate_decimates_cleanly(fs_in)) return 0;

    /* Try to restore previous selection */
    wchar_t prev[32]={0};
    ini_rs(L"S",L"DDCbw",L"",prev,32);
    int prev_sel = -1, added = 0;

    for(int D = 2; D <= fs_in/50000; D++){
        if(fs_in % D != 0) continue;
        int fs_out = fs_in / D;
        if(fs_out < 50000) break;
        if(fs_out % 100 != 0) continue;  /* skip non-round rates */

        /* When Perseus output selected, skip non-native rates */
        if(perseus_out){
            BOOL native = FALSE;
            for(int ni=0; perseus_native[ni]; ni++)
                if(fs_out == perseus_native[ni]){ native=TRUE; break; }
            if(!native) continue;
        }

        wchar_t label[32];
        if(fs_out % 1000 == 0)
            swprintf(label, 32, L"%d kHz", fs_out/1000);
        else
            swprintf(label, 32, L"%.1f kHz", (double)fs_out/1000.0);

        int idx = (int)SendMessageW(hcb, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageW(hcb, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)D);
        if(prev[0] && wcscmp(label, prev)==0) prev_sel = idx;
        added++;
    }
    if(added > 0)
        SendMessageW(hcb, CB_SETCURSEL, (WPARAM)(prev_sel>=0 ? prev_sel : 0), 0);
    return added;
}

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
    mk_group(hwnd,L"Input File",M,y,W-M*2,140);
    mk_edit(hwnd,ID_INPUT_EDIT,M+10,y+22,W-M*2-166,24,FALSE,FALSE);
    mk_btn(hwnd,ID_BROWSE,L"Browse...",W-M-154,y+22,72,24,BS_PUSHBUTTON);
    mk_btn(hwnd,ID_SEQ_ADD,L"+ Add",W-M-74,y+22,62,24,BS_PUSHBUTTON);
    /* Recording info line */
    { HWND hi=CreateWindowExW(0,L"STATIC",L"Recording:  (no file loaded)",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        M+10,y+50,W-M*2-200,20,hwnd,(HMENU)(UINT_PTR)ID_REC_INFO,g_hinst,NULL);
      SendMessageW(hi,WM_SETFONT,(WPARAM)g_font,FALSE); }
    { HWND hs=CreateWindowExW(0,L"STATIC",L"",
        WS_CHILD|WS_VISIBLE|SS_RIGHT,
        W-M-246,y+50,240,20,hwnd,(HMENU)(UINT_PTR)ID_SR_INFO,g_hinst,NULL);
      SendMessageW(hs,WM_SETFONT,(WPARAM)g_font,FALSE); }
    /* Sequence listbox */
    { HWND hl=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",NULL,
        WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        M+10,y+74,W-M*2-72,52,hwnd,(HMENU)(UINT_PTR)ID_SEQ_LIST,g_hinst,NULL);
      SendMessageW(hl,WM_SETFONT,(WPARAM)g_font_mono,FALSE); }
    mk_btn(hwnd,ID_SEQ_REMOVE,L"Remove",W-M-60,y+74,58,24,BS_PUSHBUTTON);
    EnableWindow(GetDlgItem(hwnd,ID_SEQ_REMOVE),FALSE);
    y+=148;

    /* ── Output Folder ── */
    mk_group(hwnd,L"Output Folder",M,y,W-M*2,50);
    { HWND he=mk_edit(hwnd,ID_OUTFOLDER_EDIT,M+10,y+18,W-M*2-196,22,FALSE,FALSE);
      SetWindowTextW(he,g_out_folder); }
    mk_btn(hwnd,ID_OUTFOLDER_BROWSE,L"Browse...",W-M-184,y+18,72,22,BS_PUSHBUTTON);
    mk_btn(hwnd,ID_OUTFOLDER_CLEAR, L"Same as input",W-M-108,y+18,106,22,BS_PUSHBUTTON);
    y+=58;

    /* ── Mode ── */
    mk_group(hwnd,L"Mode",M,y,W-M*2,54);
    mk_radio(hwnd,ID_MODE_FULL,L"Full file",M+10, y+26,84,20,TRUE);
    mk_radio(hwnd,ID_MODE_TRIM,L"Trim",     M+102,y+26,58,20,FALSE);
    mk_static(hwnd,L"Start:",M+170,y+28,42,16);
    mk_edit(hwnd,ID_START_EDIT,M+214,y+24,68,24,FALSE,FALSE);
    mk_static(hwnd,L"End:",M+290,y+28,34,16);
    mk_edit(hwnd,ID_END_EDIT,M+328,y+24,68,24,FALSE,FALSE);
    mk_static(hwnd,L"HHMMSS",M+400,y+28,80,16);
    y+=63;

    /* ── Channel ── */
    mk_group(hwnd,L"Channel",M,y,W-M*2,50);
    mk_radio(hwnd,ID_CHAN_DUAL,L"Dual (both tuners)",M+10, y+26,148,20,TRUE);
    mk_radio(hwnd,ID_CHAN_CH1, L"Tuner A  (--ch1)",  M+174,y+26,130,20,FALSE);
    mk_radio(hwnd,ID_CHAN_CH2, L"Tuner B  (--ch2)",  M+318,y+26,130,20,FALSE);
    y+=59;

    /* ── DDC ── */
    mk_group(hwnd,L"DDC  (Digital Down-Conversion)",M,y,W-M*2,54);
    { HWND hu=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|SS_LEFT,
        M+230,y,260,20,hwnd,(HMENU)(UINT_PTR)ID_DDC_UNAVAIL,g_hinst,NULL);
      SendMessageW(hu,WM_SETFONT,(WPARAM)g_font,FALSE); }
    mk_check(hwnd,ID_DDC_CHECK,L"Enable",M+10,y+26,72,20);
    mk_static(hwnd,L"Centre:",M+92, y+28,52,16);
    mk_edit(hwnd,ID_DDC_CENTRE,M+146,y+24,68,24,FALSE,FALSE);
    mk_static(hwnd,L"kHz",M+218,y+28,30,16);
    mk_static(hwnd,L"Bandwidth:",M+258,y+28,76,16);
    { HWND hbw=CreateWindowExW(0,L"COMBOBOX",NULL,
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
        M+336,y+24,120,200,hwnd,(HMENU)(UINT_PTR)ID_DDC_BW,
        (HINSTANCE)GetWindowLongPtrW(hwnd,GWLP_HINSTANCE),NULL);
      SetWindowPos(hbw,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE); }
    y+=63;
    mk_group(hwnd,L"Output Format",M,y,W-M*2,72);
    mk_radio(hwnd,ID_FMT_SAME,      L"Same as input", M+8,  y+22,120,20,TRUE);
    mk_radio(hwnd,ID_FMT_LINRAD,    L"Linrad",         M+136,y+22, 62,20,FALSE);
    mk_radio(hwnd,ID_FMT_WAVVIEWDX, L"WavViewDX",      M+206,y+22, 96,20,FALSE);
    mk_radio(hwnd,ID_FMT_SDRUNO,    L"SDRuno",         M+310,y+22, 70,20,FALSE);
    mk_radio(hwnd,ID_FMT_SDRCONNECT,L"SDR Connect",    M+388,y+22,106,20,FALSE);
    mk_radio(hwnd,ID_FMT_PERSEUS,   L"Perseus",        M+502,y+22, 74,20,FALSE);
    mk_radio(hwnd,ID_FMT_JAGUAR,    L"Jaguar",         M+584,y+22, 68,20,FALSE);
    /* Bit depth row — only visible for Perseus output */
    { HWND hl=CreateWindowExW(0,L"STATIC",L"Bit depth:",WS_CHILD|SS_LEFT,
        M+8,y+46,74,20,hwnd,(HMENU)(UINT_PTR)ID_BIT_LABEL,g_hinst,NULL);
      SendMessageW(hl,WM_SETFONT,(WPARAM)g_font,FALSE); }
    ShowWindow(GetDlgItem(hwnd,ID_BIT_LABEL),SW_HIDE);
    mk_radio(hwnd,ID_OUT_16BIT,L"16-bit",M+84, y+46,60,20,TRUE);
    mk_radio(hwnd,ID_OUT_24BIT,L"24-bit",M+150,y+46,60,20,FALSE);
    ShowWindow(GetDlgItem(hwnd,ID_OUT_16BIT),SW_HIDE);
    ShowWindow(GetDlgItem(hwnd,ID_OUT_24BIT),SW_HIDE);
    y+=81;

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
    mk_btn(hwnd,ID_CANCEL, L"Cancel",    W-M-170,y,78,28,BS_OWNERDRAW);
    mk_btn(hwnd,ID_CLEAR,  L"Clear Log", W-M-84, y,78,28,BS_PUSHBUTTON);
    y+=38;

    /* ── Batch section ── */
    /* Separator label */
    mk_static(hwnd,L"Batch Jobs",M,y+4,90,18);
    mk_btn(hwnd,ID_ADD_BATCH,  L"Add to Batch",W-M-280,y,96,26,BS_PUSHBUTTON);
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
    CheckRadioButton(hwnd,ID_FMT_SAME,ID_FMT_JAGUAR,ID_FMT_SAME);
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
static void browse_folder(void)
{
    BROWSEINFOW bi; memset(&bi,0,sizeof(bi));
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"Select output folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if(pidl){
        wchar_t path[MAX_PATH];
        if(SHGetPathFromIDListW(pidl,path)){
            wcsncpy(g_out_folder,path,MAX_PATH-1);
            SetWindowTextW(GetDlgItem(g_hwnd,ID_OUTFOLDER_EDIT),g_out_folder);
            update_prediction();
        }
        ILFree(pidl);
    }
}

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
        /* Clear sequence list when new primary file selected */
        g_seq_count=0;
        SendDlgItemMessageW(g_hwnd,ID_SEQ_LIST,LB_RESETCONTENT,0,0);
        EnableWindow(GetDlgItem(g_hwnd,ID_SEQ_REMOVE),FALSE);
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
        mm->ptMinTrackSize.x=800;
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
        MV(ID_INPUT_EDIT, M+10,       g_header_h+14+22,  W-M*2-166, 24)
        MV(ID_BROWSE,     W-M-154,   g_header_h+14+22,  72,        24)
        MV(ID_SEQ_ADD,    W-M-74,    g_header_h+14+22,  62,        24)
        MV(ID_REC_INFO,   M+10,      g_header_h+14+50,  W-M*2-200, 20)
        MV(ID_SR_INFO,    W-M-246,   g_header_h+14+50,  240,       20)
        MV(ID_SEQ_LIST,   M+10,      g_header_h+14+74,  W-M*2-72,  52)
        MV(ID_SEQ_REMOVE, W-M-60,    g_header_h+14+74,  58,        24)

        /* Output folder */
        MV(ID_OUTFOLDER_EDIT,    M+10,    g_header_h+14+170, W-M*2-196, 22)
        MV(ID_OUTFOLDER_BROWSE,  W-M-184, g_header_h+14+170, 72,        22)
        MV(ID_OUTFOLDER_CLEAR,   W-M-108, g_header_h+14+170, 106,       22)

        /* Prediction label stretches */
        MV(ID_OUTFILE_STATIC, M+2,  538,  W-M*2-4, 18)

        /* Log group + edit stretch */
        int log_top = 564;
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
        if((HWND)lp==GetDlgItem(hwnd,ID_DDC_UNAVAIL)){
            SetTextColor((HDC)wp,RGB(196,43,43));
            SetBkColor((HDC)wp,CLR_BG);
            return (LRESULT)g_hbr_bg;
        }
        if((HWND)lp==GetDlgItem(hwnd,ID_OUTFILE_STATIC) && g_outfile_warn){
            SetTextColor((HDC)wp,RGB(196,43,43));
            SetBkColor((HDC)wp,CLR_BG);
            return (LRESULT)g_hbr_bg;
        }
        SetBkColor((HDC)wp,CLR_BG);
        return (LRESULT)g_hbr_bg;}

    case WM_CTLCOLORBTN:
        if((HWND)lp==GetDlgItem(hwnd,ID_RUN))    return (LRESULT)NULL;
        if((HWND)lp==GetDlgItem(hwnd,ID_CANCEL)) return (LRESULT)NULL;
        SetBkColor((HDC)wp,CLR_BG);
        return (LRESULT)g_hbr_bg;

    case WM_DRAWITEM:{
        DRAWITEMSTRUCT *di=(DRAWITEMSTRUCT*)lp;
        if(di->CtlID==ID_RUN)     { draw_run_button(di);   return TRUE; }
        if(di->CtlID==ID_CANCEL)  { draw_cancel_button(di);return TRUE; }
        if(di->CtlID==ID_PROGRESS){ draw_progress_bar(di); return TRUE; }

        if(di->CtlID==ID_HEADER_PANEL){
            FillRect(di->hDC,&di->rcItem,g_hbr_header);
            SetBkMode(di->hDC,TRANSPARENT);
            SetTextColor(di->hDC,CLR_HEADER_TEXT);
            if(g_font_title) SelectObject(di->hDC,g_font_title);
            RECT tr=di->rcItem; tr.left+=14;
            DrawTextW(di->hDC,L"SDR Trim  v1.5.5",-1,&tr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
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
        case ID_OUTFOLDER_BROWSE: browse_folder(); break;
        case ID_OUTFOLDER_CLEAR:{
            g_out_folder[0]=L'\0';
            /* Show the input file's folder as a visual hint */
            wchar_t inp_cf[MAX_PATH];
            GetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),inp_cf,MAX_PATH);
            wchar_t disp[MAX_PATH]=L"";
            if(inp_cf[0]){
                wcsncpy(disp,inp_cf,MAX_PATH-1);
                wchar_t *sl=wcsrchr(disp,L'\\');
                if(sl) sl[0]=L'\0'; else disp[0]=L'\0';
            }
            SetWindowTextW(GetDlgItem(hwnd,ID_OUTFOLDER_EDIT),disp);
            update_prediction(); break;}

        case ID_SEQ_ADD:{
            if(g_seq_count >= SEQ_MAX){
                MessageBoxW(hwnd,L"Maximum sequence files reached.",L"SDR Trim",MB_OK|MB_ICONWARNING);
                break;
            }
            wchar_t buf[MAX_PATH]={0};
            OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof(ofn));
            ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=hwnd;
            ofn.lpstrFilter=L"SDRuno WAV\0*.wav\0All IQ Files\0*.raw;*.wav\0";
            ofn.lpstrFile=buf; ofn.nMaxFile=MAX_PATH;
            ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
            ofn.lpstrTitle=L"Add Sequence File";
            if(GetOpenFileNameW(&ofn)){
                /* Validate: same format/sample rate as primary */
                FileInfo fi2; memset(&fi2,0,sizeof(fi2));
                probe_file(buf,&fi2);
                FileInfo fi1; memset(&fi1,0,sizeof(fi1));
                wchar_t primary[MAX_PATH]={0};
                GetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),primary,MAX_PATH);
                if(primary[0]) probe_file(primary,&fi1);
                if(fi1.sample_rate>0 && fi2.sample_rate!=fi1.sample_rate){
                    MessageBoxW(hwnd,L"Sample rate does not match primary file.",L"SDR Trim",MB_OK|MB_ICONWARNING);
                    break;
                }
                if(fi1.fmt>=0 && fi2.fmt!=fi1.fmt){
                    MessageBoxW(hwnd,L"Format does not match primary file.",L"SDR Trim",MB_OK|MB_ICONWARNING);
                    break;
                }
                wcsncpy(g_seq_paths[g_seq_count],buf,MAX_PATH-1);
                g_seq_count++;
                /* Add short filename to listbox */
                const wchar_t *fn=wcsrchr(buf,L'\\'); fn=fn?fn+1:buf;
                SendDlgItemMessageW(hwnd,ID_SEQ_LIST,LB_ADDSTRING,0,(LPARAM)fn);
                EnableWindow(GetDlgItem(hwnd,ID_SEQ_REMOVE),TRUE);
                update_channel_controls();
                update_prediction();
            }
            break;
        }

        case ID_SEQ_REMOVE:{
            int sel=(int)SendDlgItemMessageW(hwnd,ID_SEQ_LIST,LB_GETCURSEL,0,0);
            if(sel==LB_ERR) break;
            SendDlgItemMessageW(hwnd,ID_SEQ_LIST,LB_DELETESTRING,sel,0);
            /* Shift paths down */
            for(int i=sel;i<g_seq_count-1;i++)
                wcsncpy(g_seq_paths[i],g_seq_paths[i+1],MAX_PATH-1);
            g_seq_count--;
            if(g_seq_count==0)
                EnableWindow(GetDlgItem(hwnd,ID_SEQ_REMOVE),FALSE);
            update_channel_controls();
            update_prediction();
            break;
        }
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
            update_channel_controls(); update_format_controls(); update_prediction(); break;
        case ID_FMT_SAME: case ID_FMT_LINRAD: case ID_FMT_WAVVIEWDX:
        case ID_FMT_SDRUNO: case ID_FMT_SDRCONNECT:
        case ID_FMT_PERSEUS: case ID_FMT_JAGUAR:
            /* Manually enforce mutual exclusion across full format group */
            { int fmts[]={ID_FMT_SAME,ID_FMT_LINRAD,ID_FMT_WAVVIEWDX,
                           ID_FMT_SDRUNO,ID_FMT_SDRCONNECT,ID_FMT_PERSEUS,ID_FMT_JAGUAR};
              for(int fi=0;fi<7;fi++)
                  SendDlgItemMessageW(hwnd,fmts[fi],BM_SETCHECK,
                      fmts[fi]==LOWORD(wp)?BST_CHECKED:BST_UNCHECKED,0);
            }
            update_format_controls();
            /* Set default bit depth AFTER update_format_controls so it isn't overridden */
            if(LOWORD(wp)==ID_FMT_PERSEUS){
                { FileInfo fi_bd; wchar_t inp_bd[MAX_PATH];
                  GetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),inp_bd,MAX_PATH);
                  int in_bps_bd=16; int out_sr_bd=g_sample_rate;
                  if(inp_bd[0] && probe_file(inp_bd,&fi_bd)) in_bps_bd=fi_bd.bits_per_sample;
                  if(IsDlgButtonChecked(hwnd,ID_DDC_CHECK)){
                      HWND hcb_c=GetDlgItem(hwnd,ID_DDC_BW);
                      int sel_c=(int)SendMessageW(hcb_c,CB_GETCURSEL,0,0);
                      if(sel_c>=0){ int D_c=(int)SendMessageW(hcb_c,CB_GETITEMDATA,(WPARAM)sel_c,0);
                          if(D_c>0) out_sr_bd=g_sample_rate/D_c; }
                  }
                  update_24bit_control(out_sr_bd, in_bps_bd, TRUE, TRUE); }
            }
            update_prediction(); break;
        case ID_DDC_CHECK:{
            BOOL en=IsDlgButtonChecked(hwnd,ID_DDC_CHECK);
            EnableWindow(GetDlgItem(hwnd,ID_DDC_CENTRE),en);
            EnableWindow(GetDlgItem(hwnd,ID_DDC_BW),    en);
        if(en){
            int efmt=g_fi_fmt; /* use last known input fmt */
            if(IsDlgButtonChecked(hwnd,ID_FMT_PERSEUS)) efmt=4;
            else if(IsDlgButtonChecked(hwnd,ID_FMT_JAGUAR)) efmt=5;
            else if(!IsDlgButtonChecked(hwnd,ID_FMT_SAME)) efmt=-1;
            populate_bw_dropdown(hwnd, g_sample_rate, efmt);
        }
            update_format_controls(); update_prediction(); break;}
        case ID_OUTFOLDER_EDIT:
            if(HIWORD(wp)==EN_CHANGE){
                wchar_t typed[MAX_PATH];
                GetWindowTextW(GetDlgItem(hwnd,ID_OUTFOLDER_EDIT),typed,MAX_PATH);
                /* If field is empty or matches the input file folder, use default */
                wchar_t inp_fe[MAX_PATH];
                GetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),inp_fe,MAX_PATH);
                wchar_t inp_dir[MAX_PATH]=L"";
                if(inp_fe[0]){
                    wcsncpy(inp_dir,inp_fe,MAX_PATH-1);
                    wchar_t *sl2=wcsrchr(inp_dir,L'\\');
                    if(sl2) sl2[0]=L'\0'; else inp_dir[0]=L'\0';
                }
                if(typed[0]==L'\0' || _wcsicmp(typed,inp_dir)==0)
                    g_out_folder[0]=L'\0';
                else
                    wcsncpy(g_out_folder,typed,MAX_PATH-1);
                update_prediction();
            } break;
        case ID_DDC_CENTRE:
        case ID_START_EDIT: case ID_END_EDIT: case ID_INPUT_EDIT:
            if(HIWORD(wp)==EN_CHANGE){
                update_channel_controls(); update_format_controls(); update_prediction();
                /* Refresh folder hint if using default (same as input) */
                if(g_out_folder[0]==L'\0'){
                    wchar_t inp_up[MAX_PATH];
                    GetWindowTextW(GetDlgItem(hwnd,ID_INPUT_EDIT),inp_up,MAX_PATH);
                    wchar_t dir_up[MAX_PATH]=L"";
                    if(inp_up[0]){
                        wcsncpy(dir_up,inp_up,MAX_PATH-1);
                        wchar_t *sl3=wcsrchr(dir_up,L'\\');
                        if(sl3) sl3[0]=L'\0'; else dir_up[0]=L'\0';
                    }
                    SetWindowTextW(GetDlgItem(hwnd,ID_OUTFOLDER_EDIT),dir_up);
                }
            } break;

        case ID_RUN:
            if(g_running){
                /* Job already running — this click toggles pause/resume */
                g_pause = !g_pause;
                log_append(g_pause ? L"Paused.\r\n" : L"Resumed.\r\n");
                InvalidateRect(GetDlgItem(hwnd,ID_RUN),NULL,FALSE);
                break;
            }
            {
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
                g_pause=FALSE;
                InvalidateRect(GetDlgItem(hwnd,ID_RUN),NULL,FALSE);
                EnableWindow(GetDlgItem(hwnd,ID_CANCEL),TRUE);
                InvalidateRect(GetDlgItem(hwnd,ID_CANCEL),NULL,FALSE);
                SetWindowTextW(GetDlgItem(hwnd,ID_PCT_STATIC),L"");
                SetWindowTextW(GetDlgItem(hwnd,ID_ETA_STATIC),L"");
                g_thread=CreateThread(NULL,0,run_thread,args,0,NULL);
                if(!g_thread){
                    g_running=FALSE;
                    g_pause=FALSE;
                    InvalidateRect(GetDlgItem(hwnd,ID_RUN),NULL,FALSE);
                    EnableWindow(GetDlgItem(hwnd,ID_CANCEL),FALSE);
                    InvalidateRect(GetDlgItem(hwnd,ID_CANCEL),NULL,FALSE);
                    free(args);
                }
            }
            break;

        case ID_CANCEL:
            if(g_running&&g_thread){
                g_cancel=TRUE;
                g_pause=FALSE;
                g_batch_run=FALSE;
                EnableWindow(GetDlgItem(hwnd,ID_CANCEL),FALSE);
                InvalidateRect(GetDlgItem(hwnd,ID_CANCEL),NULL,FALSE);
                log_append(L"Cancelling — waiting for current operation to stop...\r\n");
                /* Cooperative cancel only — never force-terminate the thread.
                 * TerminateThread can corrupt the CRT heap lock, causing the
                 * next file operation (even in a different job) to hang forever.
                 * The copy loops check g_cancel and will return within one
                 * read-block (well under a second for typical block sizes). */
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
                g_pause=FALSE;
                InvalidateRect(GetDlgItem(hwnd,ID_RUN),NULL,FALSE);
                EnableWindow(GetDlgItem(hwnd,ID_RUN_BATCH),FALSE);
                EnableWindow(GetDlgItem(hwnd,ID_CANCEL),TRUE);
                InvalidateRect(GetDlgItem(hwnd,ID_CANCEL),NULL,FALSE);
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
        if(nm->idFrom==ID_BATCH_LIST&&nm->code==NM_CUSTOMDRAW){
            NMLVCUSTOMDRAW *cd=(NMLVCUSTOMDRAW*)lp;
            if(cd->nmcd.dwDrawStage==CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if(cd->nmcd.dwDrawStage==CDDS_ITEMPREPAINT)
                return CDRF_NOTIFYSUBITEMDRAW;
            if(cd->nmcd.dwDrawStage==(CDDS_ITEMPREPAINT|CDDS_SUBITEM)){
                if(cd->iSubItem==COL_STATUS){
                    int idx=(int)cd->nmcd.dwItemSpec;
                    if(idx>=0&&idx<g_job_count){
                        const wchar_t *st=g_jobs[idx].status;
                        if(wcscmp(st,L"Done")==0){
                            cd->clrText=RGB(0,140,0);
                        } else if(wcscmp(st,L"Pending")==0){
                            cd->clrText=RGB(160,100,0);
                        } else if(wcscmp(st,L"Running")==0){
                            cd->clrText=RGB(0,80,180);
                        } else if(wcscmp(st,L"Failed")==0||
                                  wcscmp(st,L"Cancelled")==0){
                            cd->clrText=RGB(180,0,0);
                        }
                    }
                }
                return CDRF_NEWFONT;
            }
        }
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
            g_running=FALSE; g_pause=FALSE; g_hprocess=NULL;
            EnableWindow(GetDlgItem(hwnd,ID_RUN),TRUE);
            EnableWindow(GetDlgItem(hwnd,ID_CANCEL),FALSE);
            InvalidateRect(GetDlgItem(hwnd,ID_CANCEL),NULL,FALSE);
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

    case WM_CONFIRM_OW:{
            wchar_t *path=(wchar_t*)lp;
            wchar_t msg[MAX_PATH+80];
            wchar_t *fn=wcsrchr(path,L'\\');
            _snwprintf(msg,MAX_PATH+80,
                L"Output file already exists:\n\n%s\n\nOverwrite?",fn?fn+1:path);
            int r=MessageBoxW(hwnd,msg,L"SDR Trim - Overwrite?",
                MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);
            free(path);
            return r;
        }
    case WM_BATCHDONE:
        g_running=FALSE; g_pause=FALSE; g_batch_run=FALSE; g_hprocess=NULL;
        EnableWindow(GetDlgItem(hwnd,ID_RUN),TRUE);
        EnableWindow(GetDlgItem(hwnd,ID_CANCEL),FALSE);
        InvalidateRect(GetDlgItem(hwnd,ID_CANCEL),NULL,FALSE);
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
        populate_bw_dropdown(hwnd, g_sample_rate, g_fi_fmt);
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

    int cw=820, ch=920;
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
