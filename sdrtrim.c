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
 *   sdrtrim rec.raw 0330 1500
 *   sdrtrim rec.raw 0330 1500 --ch1 sdruno
 *   sdrtrim rec.raw 0330 1500 --ch2 sdrconnect
 *   sdrtrim rec.raw 0330 1500 wavviewdx
 *   sdrtrim rec.raw 0330 1500 --ddc 1044 500 --ch1 sdrconnect
 *   sdrtrim rec.raw --ch1 sdruno
 *   sdrtrim rec.raw --ch1
 *   sdrtrim rec.raw wavviewdx
 *
 * ── Build ─────────────────────────────────────────────────────────────
 *   Linux/macOS : gcc -O2 -Wall -o sdrtrim sdrtrim.c -lm
 *   MSYS2/MinGW : gcc -Wall -o sdrtrim.exe sdrtrim.c -lm
 */

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <time.h>

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
    FMT_UNKNOWN   = 4
} FileFormat;

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
    int64_t     epoch;              /* Unix timestamp of recording start */
    int         year, month, day;
    int         hour, minute, second;
    int         utc_offset;         /* Linrad UTC digit (0 for others)  */
    int64_t     data_offset;        /* byte offset of first sample      */
    uint64_t    data_bytes;         /* total sample data bytes          */
    char        filepath[512];
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
    double   timestamp;
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
    int yy=0,mo=0,dd=0,hh=0,mi=0,ss=0,cf_khz=0,ch=1;
    if(sscanf(bn,"SDRuno_%4d%2d%2d_%2d%2d%2d_%dkHz_ch%d",
              &yy,&mo,&dd,&hh,&mi,&ss,&cf_khz,&ch)>=7){
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
    if(got<4){ fprintf(stderr,"Error: file too small.\n"); return 0; }

    /* ── Linrad raw ── */
    if(magic[0]==0xFF && magic[1]==0xFF && magic[2]==0xFF && magic[3]==0xFF){
        uint8_t hbuf[LINRAD_HDR_SIZE];
        rewind(fp);
        if(fread(hbuf,1,LINRAD_HDR_SIZE,fp)!=LINRAD_HDR_SIZE){
            fprintf(stderr,"Error: cannot read Linrad header.\n"); return 0; }
        linrad_from_bytes(linrad_hdr_out,hbuf);
        if(linrad_hdr_out->sentinel!=-1){
            fprintf(stderr,"Error: invalid Linrad sentinel.\n"); return 0; }
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
            fprintf(stderr,"Error: RIFF file is not WAVE format.\n"); return 0; }

        /* Read chunk at offset 12 to distinguish SDRuno vs SDR Connect */
        uint8_t chunk_id[4];
        file_seek(fp,12,SEEK_SET);
        if(fread(chunk_id,1,4,fp)!=4){
            fprintf(stderr,"Error: cannot read WAV chunk.\n"); return 0; }

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
                fprintf(stderr,"Error: expected fmt chunk in SDR Connect file.\n");
                return 0; }
            uint32_t fmt_sz;
            if(fread(&fmt_sz,4,1,fp)!=1)return 0;
            FmtChunk fmt;
            if(fread(&fmt,sizeof(fmt),1,fp)!=1)return 0;
            r->sample_rate  = (int)fmt.sample_rate;
            r->num_channels = (int)fmt.num_channels;
            r->tuner        = (r->num_channels==4)?TUNER_DUAL:TUNER_SINGLE;
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
                    fprintf(stderr,
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
            r->sample_rate  = (int)fmt.sample_rate;
            r->num_channels = (int)fmt.num_channels;
            r->tuner        = (r->num_channels==4)?TUNER_DUAL:TUNER_SINGLE;
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

        fprintf(stderr,"Error: unrecognised WAV chunk layout.\n");
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
                r->sample_rate=(int)fmt.sample_rate;
                r->num_channels=(int)fmt.num_channels;
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

    fprintf(stderr,
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
    snprintf(out,sz,"%04d%02d%02d_%02d%02d%02d%d_%dkHz.raw",
             year,month,day,hh,mm,ss,utc_offset,kHz);
}

static void build_wavviewdx_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int freq_hz,int sample_rate,int out_channels)
{
    /* ch1=single-tuner(2 samples), ch2=dual-tuner(4 samples) */
    int ch = (out_channels==4) ? 2 : 1;
    snprintf(out,sz,"iq_pcm16_ch%d_cf%d_sr%d_dt%04d%02d%02d-%02d%02d%02d",
             ch,freq_hz,sample_rate,year,month,day,hh,mm,ss);
}

static void build_sdruno_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int freq_hz,OutputChan ochan)
{
    int kHz=freq_hz/1000;
    int ch=(ochan==OUT_CH2)?2:1;
    snprintf(out,sz,"SDRuno_%04d%02d%02d_%02d%02d%02d_%dkHz_ch%d.wav",
             year,month,day,hh,mm,ss,kHz,ch);
}

static void build_sdrconnect_filename(char *out, size_t sz,
    int year,int month,int day,int hh,int mm,int ss,
    int freq_hz)
{
    snprintf(out,sz,"SDRconnect_IQ_%04d%02d%02d_%02d%02d%02d_%dHZ.wav",
             year,month,day,hh,mm,ss,freq_hz);
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

static void write_fmt_pcm(FILE *fp,uint32_t sr,uint16_t ch)
{
    fwrite("fmt ",1,4,fp);
    uint32_t sz=16; fwrite(&sz,4,1,fp);
    FmtChunk fmt;
    fmt.audio_format=0x0001; fmt.num_channels=ch;
    fmt.sample_rate=sr; fmt.bits_per_sample=16;
    fmt.block_align=(uint16_t)(ch*2);
    fmt.byte_rate=sr*fmt.block_align;
    fwrite(&fmt,sizeof(fmt),1,fp);
}

/* Write SDRuno auxi chunk */
static void write_auxi(FILE *fp,int32_t cf_hz,
    int sy,int sm,int sd,int sh,int smin,
    int ey,int em,int ed,int eh,int emin,
    const char *fname)
{
    fwrite("auxi",1,4,fp);
    uint32_t sz=(uint32_t)sizeof(AuxiChunk); fwrite(&sz,4,1,fp);
    AuxiChunk a; memset(&a,0,sizeof(a));
    a.start_year=(uint16_t)sy; a.start_month=(uint16_t)sm;
    a.start_day=(uint16_t)sd;  a.start_dow=(uint16_t)day_of_week(sy,sm,sd);
    a.start_hour=(uint16_t)sh; a.start_minute=(uint16_t)smin;
    a.stop_year=(uint16_t)ey;  a.stop_month=(uint16_t)em;
    a.stop_day=(uint16_t)ed;   a.stop_dow=(uint16_t)day_of_week(ey,em,ed);
    a.stop_hour=(uint16_t)eh;  a.stop_minute=(uint16_t)emin;
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
    int32_t cf_hz,
    int sy,int sm,int sd,int sh,int smin,
    int ey,int em,int ed,int eh,int emin,
    const char *outpath,
    int *use_rf64_out, int64_t *ds64_pos_out)
{
    /* Compute total header overhead:
       SDRuno:      RIFF(12)+fmt(24)+auxi(172)+data_hdr(8) = 216  -- wait
       Actually:    RIFF(12) + fmt(8+16) + auxi(8+164) + data(8) = 216
       SDR Connect: RIFF(12) + JUNK(8+28) + fmt(8+16) + data(8) = 80
    */
    uint64_t overhead = (out_fmt==FMT_SDRUNO) ? 216 : 80;
    int use_rf64 = (out_data_bytes >= 0xFFFFFFFFULL - overhead);
    *use_rf64_out = use_rf64;
    *ds64_pos_out = -1;

    if(use_rf64){
        *ds64_pos_out = write_rf64_ds64(fp);
    } else {
        fwrite("RIFF",1,4,fp);
        uint32_t riff_sz=(uint32_t)(overhead-8+out_data_bytes);
        fwrite(&riff_sz,4,1,fp);
        fwrite("WAVE",1,4,fp);
    }

    if(out_fmt==FMT_SDRCONNECT){
        write_junk(fp);
    }

    write_fmt_pcm(fp,sample_rate,channels);

    if(out_fmt==FMT_SDRUNO){
        write_auxi(fp,cf_hz,sy,sm,sd,sh,smin,ey,em,ed,eh,emin,outpath);
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
                          int out_ch_count)
{
    int64_t total=file_tell(fp);
    uint64_t actual_data=(uint64_t)(total-(data_sz32_pos+4));
    uint64_t actual_frames=actual_data/((uint64_t)out_ch_count*2);

    if(use_rf64){
        patch_rf64(fp,ds64_pos,data_sz32_pos,
                   (uint64_t)total,actual_data,actual_frames);
    } else {
        file_seek(fp,4,SEEK_SET);
        uint32_t riff_sz=(uint32_t)((uint64_t)total-8);
        fwrite(&riff_sz,4,1,fp);
        file_seek(fp,data_sz32_pos,SEEK_SET);
        uint32_t data_sz=(uint32_t)actual_data;
        fwrite(&data_sz,4,1,fp);
    }
}

/* ------------------------------------------------------------------ */
/*  Progress bar                                                       */
/* ------------------------------------------------------------------ */
#define PROGRESS_WIDTH 55
static time_t _prog_start = 0;

static void progress_print(const char *label,uint64_t done,uint64_t total)
{
    if(done==0) _prog_start=time(NULL);
    double frac=(total>0)?(double)done/(double)total:0.0;
    int filled=(int)(frac*PROGRESS_WIDTH);
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
    printf("\r%-12s [",label);
    for(int i=0;i<PROGRESS_WIDTH;i++) putchar(i<filled?'#':'-');
    printf("] %3d%%  %s",(int)(frac*100.0),eta);
    fflush(stdout);
}
static void progress_done(void){ printf("\n"); fflush(stdout); }

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
/*  DDC block processing                                               */
/* ------------------------------------------------------------------ */
#define COPY_BUF_FRAMES 65536
#define CLIP16(x) ((int16_t)((x)>32767?32767:((x)<-32768?-32768:(int16_t)((x)+0.5))))

static int ddc_block(
    const int16_t *in, int n_in, int in_ch,
    const double *h, int n_taps, int D,
    double delta_phi, double *phi,
    double *zI_a, double *zQ_a,
    double *zI_b, double *zQ_b,
    OutputChan ochan, int out_ch,
    int16_t *out_buf)
{
    int overlap=n_taps-1;
    int n_out=0;
    for(int i=0;i<n_in;i++){
        double IA=(double)in[i*in_ch+0];
        double QA=(double)in[i*in_ch+1];
        double IB=0.0,QB=0.0;
        if(in_ch==4){
            IB=(double)in[i*in_ch+2];
            QB=(double)in[i*in_ch+3];
        }

        double c=cos(*phi), s=sin(*phi);
        *phi+=delta_phi;
        if(*phi>M_PI)  *phi-=2.0*M_PI;
        if(*phi<-M_PI) *phi+=2.0*M_PI;

        double IA2=IA*c-QA*s, QA2=IA*s+QA*c;
        double IB2=IB*c-QB*s, QB2=IB*s+QB*c;

        memmove(zI_a+1,zI_a,(size_t)(overlap-1)*sizeof(double));
        memmove(zQ_a+1,zQ_a,(size_t)(overlap-1)*sizeof(double));
        zI_a[0]=IA2; zQ_a[0]=QA2;

        if(out_ch==4 || ochan==OUT_CH2){
            memmove(zI_b+1,zI_b,(size_t)(overlap-1)*sizeof(double));
            memmove(zQ_b+1,zQ_b,(size_t)(overlap-1)*sizeof(double));
            zI_b[0]=IB2; zQ_b[0]=QB2;
        }

        if(i%D==0){
            double oIA=0,oQA=0,oIB=0,oQB=0;
            for(int k=0;k<n_taps;k++){ oIA+=h[k]*zI_a[k]; oQA+=h[k]*zQ_a[k]; }
            if(out_ch==4||ochan==OUT_CH2){
                for(int k=0;k<n_taps;k++){ oIB+=h[k]*zI_b[k]; oQB+=h[k]*zQ_b[k]; }
            }

            if(out_ch==4){
                out_buf[n_out*4+0]=CLIP16(oIA); out_buf[n_out*4+1]=CLIP16(oQA);
                out_buf[n_out*4+2]=CLIP16(oIB); out_buf[n_out*4+3]=CLIP16(oQB);
            } else if(ochan==OUT_CH2){
                out_buf[n_out*2+0]=CLIP16(oIB); out_buf[n_out*2+1]=CLIP16(oQB);
            } else {
                out_buf[n_out*2+0]=CLIP16(oIA); out_buf[n_out*2+1]=CLIP16(oQA);
            }
            n_out++;
        }
    }
    return n_out;
}

/* ------------------------------------------------------------------ */
/*  Streaming copy (no DDC)                                            */
/* ------------------------------------------------------------------ */
static int copy_passthrough(FILE *fin,FILE *fout,
    uint64_t total_frames, int in_ch, int out_ch,
    int src_idx, /* 0=chA, 2=chB, -1=all */
    const char *label)
{
    int16_t *ibuf=(int16_t*)malloc(COPY_BUF_FRAMES*(size_t)in_ch*2);
    int16_t *obuf=(int16_t*)malloc(COPY_BUF_FRAMES*(size_t)out_ch*2);
    if(!ibuf||!obuf){ perror("malloc"); free(ibuf);free(obuf); return -1; }

    uint64_t left=total_frames, done=0;
    progress_print(label,0,total_frames);
    while(left>0){
        uint64_t batch=(left>(uint64_t)COPY_BUF_FRAMES)?(uint64_t)COPY_BUF_FRAMES:left;
        size_t got=fread(ibuf,sizeof(int16_t),(size_t)batch*(size_t)in_ch,fin);
        if(got==0){ fprintf(stderr,"\nWarning: source ended early.\n"); break; }
        size_t gf=got/(size_t)in_ch;
        if(src_idx<0){
            if(fwrite(ibuf,sizeof(int16_t),got,fout)!=got){
                perror("\nfwrite"); free(ibuf);free(obuf); return -1; }
        } else {
            for(size_t f=0;f<gf;f++){
                obuf[f*2+0]=ibuf[f*in_ch+src_idx];
                obuf[f*2+1]=ibuf[f*in_ch+src_idx+1];
            }
            if(fwrite(obuf,sizeof(int16_t),gf*2,fout)!=gf*2){
                perror("\nfwrite"); free(ibuf);free(obuf); return -1; }
        }
        done+=gf; left-=gf;
        progress_print(label,done,total_frames);
    }
    free(ibuf); free(obuf);
    progress_done();
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Streaming DDC copy                                                 */
/* ------------------------------------------------------------------ */
static int copy_ddc(FILE *fin,FILE *fout,
    uint64_t in_frames, int in_ch, int out_ch,
    const double *h, int n_taps, int D,
    double delta_phi, OutputChan ochan,
    const char *label)
{
    int overlap=n_taps-1;
    double *zI_a=(double*)calloc((size_t)overlap,sizeof(double));
    double *zQ_a=(double*)calloc((size_t)overlap,sizeof(double));
    double *zI_b=(double*)calloc((size_t)overlap,sizeof(double));
    double *zQ_b=(double*)calloc((size_t)overlap,sizeof(double));
    int16_t *ibuf=(int16_t*)malloc(COPY_BUF_FRAMES*(size_t)in_ch*2);
    int16_t *obuf=(int16_t*)malloc(((size_t)COPY_BUF_FRAMES/D+2)*(size_t)out_ch*2);
    if(!zI_a||!zQ_a||!zI_b||!zQ_b||!ibuf||!obuf){
        perror("malloc");
        free(zI_a);free(zQ_a);free(zI_b);free(zQ_b);free(ibuf);free(obuf);
        return -1;
    }

    double phi=0.0;
    uint64_t left=in_frames, done=0;
    progress_print(label,0,in_frames);

    while(left>0){
        uint64_t batch=(left>(uint64_t)COPY_BUF_FRAMES)?(uint64_t)COPY_BUF_FRAMES:left;
        size_t got=fread(ibuf,sizeof(int16_t),(size_t)batch*(size_t)in_ch,fin);
        if(got==0){ fprintf(stderr,"\nWarning: source ended early.\n"); break; }
        int gf=(int)(got/(size_t)in_ch);
        int n_out=ddc_block(ibuf,gf,in_ch,h,n_taps,D,delta_phi,&phi,
                            zI_a,zQ_a,zI_b,zQ_b,ochan,out_ch,obuf);
        if(n_out>0){
            size_t samps=(size_t)n_out*(size_t)out_ch;
            if(fwrite(obuf,sizeof(int16_t),samps,fout)!=samps){
                perror("\nfwrite");
                free(zI_a);free(zQ_a);free(zI_b);free(zQ_b);free(ibuf);free(obuf);
                return -1;
            }
        }
        done+=(uint64_t)gf; left-=(uint64_t)gf;
        progress_print(label,done,in_frames);
    }
    free(zI_a);free(zQ_a);free(zI_b);free(zQ_b);free(ibuf);free(obuf);
    progress_done();
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Print usage                                                        */
/* ------------------------------------------------------------------ */
static void print_usage(const char *prog)
{
    fprintf(stderr,
"SDR Trim  -  IQ Recording Trim, Convert and Frequency-Extract Utility\n"
"\n"
"Usage:\n"
"  %s <input> <start_HHMM> <end_HHMM> [options]   Trim\n"
"  %s <input> [options]                             Full file\n"
"\n"
"Options:\n"
"  --ch1              Extract tuner A as single-tuner output\n"
"  --ch2              Extract tuner B as single-tuner output\n"
"                     (omit for dual-tuner output from dual-tuner input)\n"
"  linrad             Output format: Linrad raw (.raw)\n"
"  wavviewdx          Output format: WavViewDX raw (.raw, headerless)\n"
"  sdruno             Output format: SDRuno WAV (.wav)\n"
"  sdrconnect         Output format: SDR Connect WAV (.wav)\n"
"                     Default: same as input format\n"
"                     (--fmt <format> also accepted for compatibility)\n"
"  --ddc <kHz> <bw>   DDC: extract frequency slice at <kHz>, bandwidth <bw> kHz\n"
"\n"
"Format rules:\n"
"  Dual-tuner output  : linrad and wavviewdx only\n"
"  Single-tuner output: all four formats\n"
"\n"
"Midnight crossing is handled automatically.\n"
"\n"
"Examples:\n"
"  %s rec.raw 0330 1500\n"
"  %s rec.raw 0330 1500 --ch1\n"
"  %s rec.raw 0330 1500 --ch1 sdrconnect\n"
"  %s rec.raw 0330 1500 --ch2 sdruno\n"
"  %s rec.raw 0330 1500 wavviewdx\n"
"  %s rec.raw 0330 1500 --ddc 1044 500 --ch1 sdrconnect\n"
"  %s rec.raw 0330 1500 --ddc 1044 500\n"
"  %s rec.raw --ch1 sdruno\n"
"  %s rec.raw --ch1\n"
"  %s rec.raw wavviewdx\n"
"  %s rec.raw --ddc 1044 500\n",
    prog,prog,prog,prog,prog,prog,prog,prog,prog,prog,prog,prog,prog);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if(argc<3){ print_usage(argv[0]); return EXIT_FAILURE; }

    const char *inpath = argv[1];

    /* ── Parse positional args and options ── */
    int      do_convert  = 0;
    int      req_start_hh=0,req_start_mm=0;
    int      req_end_hh=0,  req_end_mm=0;
    OutputChan ochan     = OUT_DUAL;     /* default: keep all channels */
    FileFormat out_fmt   = FMT_UNKNOWN;  /* resolved after input detect */
    int      do_ddc      = 0;
    int      ddc_freq_khz= 0;
    int      ddc_bw_khz  = 0;
    int      ochan_set   = 0;           /* user explicitly set ch1/ch2 */

    /* Second arg: HHMM start time, --convert, or start of options (implicit full-file) */
    if(strcmp(argv[2],"--convert")==0){
        do_convert=1;
    } else if(strlen(argv[2])==4
              && sscanf(argv[2],"%2d%2d",&req_start_hh,&req_start_mm)==2
              && req_start_hh<=23 && req_start_mm<=59){
        /* argv[2] looks like a valid HHMM time  -  argv[3] must also be a valid HHMM */
        if(argc<4){
            fprintf(stderr,"Error: start time %s given but no end time.\n"
                    "  Usage: sdrtrim <input> <start_HHMM> <end_HHMM> [options]\n",
                    argv[2]);
            return EXIT_FAILURE;
        }
        if(strlen(argv[3])!=4
           || sscanf(argv[3],"%2d%2d",&req_end_hh,&req_end_mm)!=2
           || req_end_hh>23 || req_end_mm>59){
            fprintf(stderr,"Error: start time %s given but end time '%s' is not valid HHMM.\n"
                    "  Both start and end must be 4-digit times, e.g. 0300 1200.\n"
                    "  For full-file processing omit both time arguments.\n",
                    argv[2], argv[3]);
            return EXIT_FAILURE;
        }
        /* Trim mode confirmed */
    } else {
        /* No HHMM and no --convert: implicit full-file mode.
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
        else if(strcmp(argv[i],"linrad"    )==0) out_fmt=FMT_LINRAD;
        else if(strcmp(argv[i],"wavviewdx" )==0) out_fmt=FMT_WAVVIEWDX;
        else if(strcmp(argv[i],"sdruno"    )==0) out_fmt=FMT_SDRUNO;
        else if(strcmp(argv[i],"sdrconnect")==0) out_fmt=FMT_SDRCONNECT;
        else if(strcmp(argv[i],"--fmt")==0){
            /* --fmt <format> accepted for backward compatibility */
            if(i+1>=argc){ fprintf(stderr,"Error: --fmt requires a format name\n");
                return EXIT_FAILURE; }
            i++;
            if     (strcmp(argv[i],"linrad"    )==0) out_fmt=FMT_LINRAD;
            else if(strcmp(argv[i],"wavviewdx" )==0) out_fmt=FMT_WAVVIEWDX;
            else if(strcmp(argv[i],"sdruno"    )==0) out_fmt=FMT_SDRUNO;
            else if(strcmp(argv[i],"sdrconnect")==0) out_fmt=FMT_SDRCONNECT;
            else{ fprintf(stderr,"Error: unknown format '%s'\n",argv[i]);
                  return EXIT_FAILURE; }
        }
        else if(strcmp(argv[i],"--ddc")==0){
            if(i+2>=argc){ fprintf(stderr,"Error: --ddc requires <kHz> <bw_kHz>\n");
                return EXIT_FAILURE; }
            do_ddc=1;
            ddc_freq_khz=atoi(argv[i+1]);
            ddc_bw_khz  =atoi(argv[i+2]);
            if(ddc_freq_khz<=0||ddc_bw_khz<=0){
                fprintf(stderr,"Error: DDC frequencies must be positive integers\n");
                return EXIT_FAILURE; }
            i+=2;
        }
        else if(strcmp(argv[i],"--convert")==0){ /* already handled */ }
        else{ fprintf(stderr,"Error: unrecognised argument '%s'\n"
                   "  Expected: HHMM time, format word (linrad/wavviewdx/sdruno/sdrconnect),\n"
                   "  --ch1, --ch2, --ddc <kHz> <bw>, or --convert\n",argv[i]);
              return EXIT_FAILURE; }
    }

    /* ── Open input ── */
    FILE *fin=fopen(inpath,"rb");
    if(!fin){ fprintf(stderr,"Error opening '%s': %s\n",inpath,strerror(errno));
              return EXIT_FAILURE; }

    RecInfo rec;
    LinradHdr linrad_hdr;
    memset(&linrad_hdr,0,sizeof(linrad_hdr));
    if(!detect_input(inpath,&rec,fin,&linrad_hdr)){
        fclose(fin); return EXIT_FAILURE; }

    /* ── Resolve output channel mode ── */
    if(!ochan_set){
        /* No --ch1/--ch2: default to dual if input is dual, single otherwise */
        ochan = (rec.tuner==TUNER_DUAL) ? OUT_DUAL : OUT_CH1;
    }

    /* Validate: can't request ch2 from single-tuner input */
    if(ochan==OUT_CH2 && rec.tuner==TUNER_SINGLE){
        fprintf(stderr,
            "Error: --ch2 requested but input is single-tuner (no second tuner).\n");
        fclose(fin); return EXIT_FAILURE;
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
    if(out_is_dual && (out_fmt==FMT_SDRUNO||out_fmt==FMT_SDRCONNECT)){
        fprintf(stderr,
            "Error: dual-tuner output only supports linrad and wavviewdx formats.\n"
            "  Use --ch1 or --ch2 to extract a single tuner for WAV output.\n");
        fclose(fin); return EXIT_FAILURE;
    }

    /* ── Format name strings (used in messages below) ── */
    const char *fmt_names[]={"Linrad raw","WavViewDX raw","SDRuno WAV","SDR Connect WAV","Unknown"};

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
            printf("Note: input and output are the same format (%s) with no trim, DDC,\n"
                   "  or channel extraction  -  no conversion required.\n",
                   fmt_names[rec.fmt]);
            fclose(fin); return EXIT_SUCCESS;
        }
    }

    /* ── Print input info ── */
    printf("Input       : %s\n",inpath);
    printf("Format      : %s\n",fmt_names[rec.fmt]);
    printf("Tuner       : %s\n",rec.tuner==TUNER_DUAL?"Dual (IA,QA,IB,QB)":"Single (I,Q)");
    printf("Sample rate : %d Hz\n",rec.sample_rate);
    printf("Centre freq : %d Hz = %.3f kHz\n",rec.centre_freq_hz,rec.centre_freq_hz/1000.0);
    printf("Start time  : %04d-%02d-%02d %02d:%02d:%02d UTC\n",
           rec.year,rec.month,rec.day,rec.hour,rec.minute,rec.second);

    uint64_t total_secs=rec.data_bytes/((uint64_t)rec.num_channels*2*(uint64_t)rec.sample_rate);
    printf("Duration    : %llu s\n",(unsigned long long)total_secs);
    printf("Output mode : %s -> %s\n",
           ochan==OUT_DUAL?"Dual-tuner":ochan==OUT_CH1?"Tuner A (ch1)":"Tuner B (ch2)",
           fmt_names[out_fmt]);

    /* ── Resolve trim / convert times ── */
    int64_t file_epoch = rec.epoch;
    int out_year=rec.year, out_month=rec.month, out_day=rec.day;
    int64_t start_epoch, end_epoch;

    if(do_convert){
        /* No trim  -  process entire file */
        start_epoch = file_epoch;
        end_epoch   = file_epoch + (int64_t)total_secs;
        out_year=rec.year; out_month=rec.month; out_day=rec.day;
    } else {
        /* Resolve start */
        start_epoch=utc_to_unix(out_year,out_month,out_day,req_start_hh,req_start_mm,0);
        if(start_epoch<file_epoch){
            next_day(&out_year,&out_month,&out_day);
            start_epoch=utc_to_unix(out_year,out_month,out_day,req_start_hh,req_start_mm,0);
            printf("Note: start %02d:%02d past midnight  -  using %04d-%02d-%02d\n",
                   req_start_hh,req_start_mm,out_year,out_month,out_day);
        }
        if(start_epoch<file_epoch){
            fprintf(stderr,"Error: start time is before file start.\n");
            fclose(fin); return EXIT_FAILURE; }

        /* Resolve end */
        int end_year=out_year, end_month=out_month, end_day=out_day;
        end_epoch=utc_to_unix(end_year,end_month,end_day,req_end_hh,req_end_mm,0);
        if(end_epoch<=start_epoch){
            next_day(&end_year,&end_month,&end_day);
            end_epoch=utc_to_unix(end_year,end_month,end_day,req_end_hh,req_end_mm,0);
            printf("Note: end %02d:%02d <= start  -  using %04d-%02d-%02d\n",
                   req_end_hh,req_end_mm,end_year,end_month,end_day);
        }

        int64_t file_end=file_epoch+(int64_t)total_secs;
        if(end_epoch>file_end){
            int fe_hh,fe_mm,fe_ss,fe_yy,fe_mo,fe_dd;
            unix_to_utc(file_end,&fe_yy,&fe_mo,&fe_dd,&fe_hh,&fe_mm,&fe_ss);
            fprintf(stderr,"Error: end time %02d:%02d is beyond the end of the recording.\n"
                    "  Recording ends at %02d:%02d UTC on %04d-%02d-%02d.\n",
                    req_end_hh,req_end_mm,fe_hh,fe_mm,fe_yy,fe_mo,fe_dd);
            fclose(fin); return EXIT_FAILURE; }
    }

    int64_t duration_sec  = end_epoch - start_epoch;
    if(duration_sec<=0){
        fprintf(stderr,"Error: zero or negative duration.\n");
        fclose(fin); return EXIT_FAILURE; }

    uint64_t start_frame =(uint64_t)(start_epoch-file_epoch)*(uint64_t)rec.sample_rate;
    uint64_t in_frames   =(uint64_t)duration_sec*(uint64_t)rec.sample_rate;

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
        if(!fir_h){ perror("malloc FIR"); fclose(fin); return EXIT_FAILURE; }

        delta_phi=2.0*M_PI*(double)(rec.centre_freq_hz-ddc_cf_hz)/(double)rec.sample_rate;

        printf("DDC centre  : %d kHz\n",ddc_freq_khz);
        printf("DDC BW      : %d kHz\n",ddc_bw_khz);
        printf("Decimation  : %d -> output rate %d Hz\n",ddc_D,fs_out);
        printf("FIR taps    : %d\n",n_taps);
    }

    uint64_t out_frames = in_frames/(uint64_t)(do_ddc?ddc_D:1);

    /* ── Output channel count ── */
    int out_ch = (ochan==OUT_DUAL) ? rec.num_channels : 2;

    /* Trim info */
    int trim_sh=req_start_hh, trim_sm=req_start_mm;
    int trim_eh=req_end_hh,   trim_em=req_end_mm;
    if(do_convert){
        trim_sh=rec.hour; trim_sm=rec.minute;
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
    char outpath[512];
    switch(out_fmt){
        case FMT_LINRAD:
            build_linrad_filename(outpath,sizeof(outpath),
                out_year,out_month,out_day,
                trim_sh,trim_sm,0,
                rec.utc_offset,ddc_cf_hz,out_ch);
            break;
        case FMT_WAVVIEWDX:
            build_wavviewdx_filename(outpath,sizeof(outpath),
                out_year,out_month,out_day,
                trim_sh,trim_sm,0,
                ddc_cf_hz,fs_out,out_ch);
            /* append .raw */
            { size_t n=strlen(outpath);
              if(n+4<sizeof(outpath)) strcat(outpath,".raw"); }
            break;
        case FMT_SDRUNO:
            build_sdruno_filename(outpath,sizeof(outpath),
                out_year,out_month,out_day,
                trim_sh,trim_sm,0,
                ddc_cf_hz,ochan);
            break;
        case FMT_SDRCONNECT:
            build_sdrconnect_filename(outpath,sizeof(outpath),
                out_year,out_month,out_day,
                trim_sh,trim_sm,0,
                ddc_cf_hz);
            break;
        default: break;
    }

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
            printf("Note: output name matched input -- renamed to: %s\n",outpath);
        }
    }
    printf("Output file : %s\n\n",outpath);

    /* ── Seek input to trim start ── */
    uint64_t skip_bytes=start_frame*(uint64_t)rec.num_channels*2;
    if(file_seek(fin,rec.data_offset+(int64_t)skip_bytes,SEEK_SET)!=0){
        fprintf(stderr,"Error seeking: %s\n",strerror(errno));
        free(fir_h); fclose(fin); return EXIT_FAILURE; }

    /* ── Open output ── */
    FILE *fout=fopen(outpath,"wb");
    if(!fout){
        fprintf(stderr,"Error creating '%s': %s\n",outpath,strerror(errno));
        free(fir_h); fclose(fin); return EXIT_FAILURE; }

    /* ── Write output header ── */
    int64_t ds64_pos=-1, data_sz32_pos=-1;
    int use_rf64=0;
    uint64_t out_data_bytes=out_frames*(uint64_t)out_ch*2;

    int end_yy,end_mm2,end_dd,end_hh2,end_mi2,end_ss2;
    unix_to_utc(end_epoch,&end_yy,&end_mm2,&end_dd,&end_hh2,&end_mi2,&end_ss2);

    switch(out_fmt){
        case FMT_LINRAD:{
            LinradHdr oh=linrad_hdr;
            /* For non-Linrad input, populate fields from detected metadata */
            oh.sentinel=-1;
            oh.timestamp=(double)start_epoch;
            oh.passband_center=(double)ddc_cf_hz/1e6;
            oh.rx_rf_channels=(out_ch==4)?2:1;
            oh.rx_ad_channels=out_ch;
            oh.rx_ad_speed=fs_out;
            /* preserve passband_direction, rx_input_mode, save_init_flag from input
               (zeroed for non-Linrad input) */
            uint8_t hbuf[LINRAD_HDR_SIZE];
            linrad_to_bytes(&oh,hbuf);
            if(fwrite(hbuf,1,LINRAD_HDR_SIZE,fout)!=LINRAD_HDR_SIZE){
                fprintf(stderr,"Error writing header.\n"); goto write_error; }
            break; }
        case FMT_WAVVIEWDX:
            /* Headerless  -  nothing to write */
            break;
        case FMT_SDRUNO:
        case FMT_SDRCONNECT:
            data_sz32_pos=write_wav_header(fout,out_fmt,out_data_bytes,
                (uint32_t)fs_out,(uint16_t)out_ch,(int32_t)ddc_cf_hz,
                out_year,out_month,out_day,trim_sh,trim_sm,
                end_yy,end_mm2,end_dd,end_hh2,end_mi2,
                outpath,&use_rf64,&ds64_pos);
            break;
        default: break;
    }

    /* ── Process and stream data ── */
    int src_idx = (ochan==OUT_CH2)?2:(ochan==OUT_CH1)?0:-1;
    const char *label = do_ddc ? "DDC+Write " : "Writing   ";

    int rc;
    if(do_ddc){
        rc=copy_ddc(fin,fout,in_frames,rec.num_channels,out_ch,
                    fir_h,n_taps,ddc_D,delta_phi,ochan,label);
    } else {
        rc=copy_passthrough(fin,fout,in_frames,rec.num_channels,out_ch,
                            src_idx,label);
    }
    if(rc!=0) goto write_error;

    /* ── Finalise WAV headers ── */
    if(out_fmt==FMT_SDRUNO||out_fmt==FMT_SDRCONNECT){
        finalise_wav(fout,use_rf64,ds64_pos,data_sz32_pos,out_ch);
    }

    fclose(fin); fclose(fout); free(fir_h);
    printf("Done. Output: %s\n",outpath);
    return EXIT_SUCCESS;

write_error:
    fprintf(stderr,"Fatal error  -  removing partial output.\n");
    fclose(fin); fclose(fout); free(fir_h);
    remove(outpath);
    return EXIT_FAILURE;
}
