#include "net/tls13.h"
#include "x86_64/kcrypto.h"
#include "x86_64/random.h"
#include "x86_64/allocator.h"
#include "time.h"
#include "string.h"
#include "serial.h"

#define TLS13_MAX_RECORD 18432u
#define TLS13_MAX_TRANSCRIPT 32768u
#define TLS13_TIMEOUT 15000u

struct tls13_client {
    tcp_conn_t* conn;
    uint8_t transcript[TLS13_MAX_TRANSCRIPT];
    uint32_t transcript_len;
    uint8_t hs_buf[TLS13_MAX_TRANSCRIPT];
    uint32_t hs_len;
    uint8_t record[TLS13_MAX_RECORD];
    uint32_t record_len;
    uint8_t client_hs_key[16], client_hs_iv[12];
    uint8_t server_hs_key[16], server_hs_iv[12];
    uint8_t client_app_key[16], client_app_iv[12];
    uint8_t server_app_key[16], server_app_iv[12];
    uint64_t tx_seq, rx_seq;
    bool established;
};

static const char* g_tls13_error = "unknown";
const char* tls13_last_error(void) { return g_tls13_error; }

static const char* tls_alert_name(uint8_t description) {
    switch (description) {
        case 40: return "handshake failure";
        case 47: return "illegal parameter";
        case 50: return "decode error";
        case 70: return "protocol version";
        case 109: return "missing extension";
        default: return "unknown alert";
    }
}

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint32_t be24(const uint8_t* p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static void put16(uint8_t* p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void put24(uint8_t* p, uint32_t v) { p[0]=(uint8_t)(v>>16); p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)v; }

static bool send_all(tcp_conn_t* c, const uint8_t* p, uint32_t len) {
    uint32_t sent=0; uint64_t last=time_get_uptime_ms();
    while (sent<len) {
        int32_t n=tcp_send(c,p+sent,len-sent,5000);
        if (n<0) return false;
        if (n==0) { if (time_get_uptime_ms()-last>6000) return false; continue; }
        sent+=(uint32_t)n; last=time_get_uptime_ms();
    }
    return true;
}

static bool recv_all(tcp_conn_t* c, uint8_t* p, uint32_t len, uint32_t timeout) {
    uint32_t got=0; uint64_t start=time_get_uptime_ms();
    while (got<len) {
        int32_t n=tcp_recv(c,p+got,len-got);
        if (n<0) return false;
        if (n==0) { if (time_get_uptime_ms()-start>timeout) return false; tcp_poll(); continue; }
        got+=(uint32_t)n; start=time_get_uptime_ms();
    }
    return true;
}

/* SHA-256 HMAC and HKDF are kept local because TLS uses SHA-256 only. */
static void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg,
                        size_t msg_len, uint8_t out[32]) {
    uint8_t k[64]={0}, ipad[64], opad[64], inner[32];
    if (key_len>64) { kcrypto_sha256(key,key_len,k); key_len=32; }
    else memcpy(k,key,key_len);
    for (int i=0;i<64;i++) { ipad[i]=(uint8_t)(k[i]^0x36); opad[i]=(uint8_t)(k[i]^0x5c); }
    kcrypto_sha256_ctx h; kcrypto_sha256_init(&h); kcrypto_sha256_update(&h,ipad,64);
    kcrypto_sha256_update(&h,msg,msg_len); kcrypto_sha256_final(&h,inner);
    kcrypto_sha256_init(&h); kcrypto_sha256_update(&h,opad,64);
    kcrypto_sha256_update(&h,inner,32); kcrypto_sha256_final(&h,out);
}

static void hkdf_extract(const uint8_t* salt, size_t salt_len, const uint8_t* ikm,
                         size_t ikm_len, uint8_t out[32]) {
    uint8_t zero[32]={0};
    hmac_sha256(salt_len?salt:zero, salt_len?salt_len:32, ikm, ikm_len, out);
}

static void hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t info_len,
                        uint8_t* out, size_t out_len) {
    uint8_t t[32]={0}; size_t done=0; uint8_t block[256]; uint8_t counter=1;
    while (done<out_len) {
        size_t n=0; if (counter!=1) { memcpy(block,t,32); n=32; }
        memcpy(block+n,info,info_len); n+=info_len; block[n++]=counter++;
        hmac_sha256(prk,32,block,n,t);
        size_t take=out_len-done; if (take>32) take=32;
        memcpy(out+done,t,take); done+=take;
    }
}

static void hkdf_label(const uint8_t secret[32], const char* label,
                       const uint8_t* context, size_t context_len,
                       uint8_t* out, size_t out_len) {
    uint8_t info[96]; size_t label_len=0; while(label[label_len]) label_len++;
    size_t full=6+label_len;
    put16(info,(uint16_t)out_len); info[2]=(uint8_t)full;
    memcpy(info+3,"tls13 ",6); memcpy(info+9,label,label_len);
    info[3+full]=(uint8_t)context_len; memcpy(info+4+full,context,context_len);
    hkdf_expand(secret,info,4+full+context_len,out,out_len);
}

static void transcript_hash(const tls13_client_t* c, uint8_t out[32]) {
    kcrypto_sha256(c->transcript,c->transcript_len,out);
}

static void transcript_add(tls13_client_t* c, const uint8_t* p, uint32_t len) {
    if (len<=TLS13_MAX_TRANSCRIPT-c->transcript_len) { memcpy(c->transcript+c->transcript_len,p,len); c->transcript_len+=len; }
}

/* X25519, RFC 7748.  This is the well-tested 16x16-bit Montgomery ladder
 * representation; it keeps all intermediates within signed 64-bit bounds. */
typedef int64_t x25519_fe[16];

static void x25519_carry(x25519_fe a) {
    for (int i = 0; i < 16; i++) {
        a[i] += (int64_t)1 << 16;
        int64_t carry = a[i] >> 16;
        a[(i + 1) & 15] += carry - 1 + (i == 15 ? 37 * (carry - 1) : 0);
        a[i] -= carry << 16;
    }
}
static void x25519_select(x25519_fe a, x25519_fe b, uint8_t swap) {
    int64_t mask = -(int64_t)swap;
    for (int i = 0; i < 16; i++) { int64_t t = mask & (a[i] ^ b[i]); a[i] ^= t; b[i] ^= t; }
}
static void x25519_add(x25519_fe out, const x25519_fe a, const x25519_fe b) { for (int i=0;i<16;i++) out[i]=a[i]+b[i]; }
static void x25519_sub(x25519_fe out, const x25519_fe a, const x25519_fe b) { for (int i=0;i<16;i++) out[i]=a[i]-b[i]; }
static void x25519_mul(x25519_fe out, const x25519_fe a, const x25519_fe b) {
    int64_t t[31] = {0};
    for (int i=0;i<16;i++) for (int j=0;j<16;j++) t[i+j] += a[i]*b[j];
    for (int i=0;i<15;i++) t[i] += 38*t[i+16];
    for (int i=0;i<16;i++) out[i]=t[i];
    x25519_carry(out); x25519_carry(out);
}
static void x25519_square(x25519_fe out, const x25519_fe a) { x25519_mul(out,a,a); }
static void x25519_invert(x25519_fe out, const x25519_fe in) {
    x25519_fe c; memcpy(c,in,sizeof(c));
    for (int i=253;i>=0;i--) { x25519_square(c,c); if (i != 2 && i != 4) x25519_mul(c,c,in); }
    memcpy(out,c,sizeof(c));
}
static void x25519_unpack(x25519_fe out, const uint8_t in[32]) {
    for (int i=0;i<16;i++) out[i]=in[2*i]+((int64_t)in[2*i+1]<<8);
    out[15] &= 0x7fff;
}
static void x25519_pack(uint8_t out[32], const x25519_fe in) {
    x25519_fe t,m; memcpy(t,in,sizeof(t)); x25519_carry(t); x25519_carry(t); x25519_carry(t);
    for (int j=0;j<2;j++) {
        m[0]=t[0]-0xffed;
        for (int i=1;i<15;i++) { m[i]=t[i]-0xffff-((m[i-1]>>16)&1); m[i-1]&=0xffff; }
        m[15]=t[15]-0x7fff-((m[14]>>16)&1);
        uint8_t choose_m=(uint8_t)(1-((m[15]>>16)&1)); m[14]&=0xffff; x25519_select(t,m,choose_m);
    }
    for (int i=0;i<16;i++) { out[2*i]=(uint8_t)t[i]; out[2*i+1]=(uint8_t)(t[i]>>8); }
}
static void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    static const x25519_fe a24 = {0xdb41, 1};
    uint8_t scalar_copy[32]; x25519_fe x,a,b,c,d,e,f;
    memcpy(scalar_copy,scalar,32); scalar_copy[0]&=248; scalar_copy[31]&=127; scalar_copy[31]|=64;
    x25519_unpack(x,point); memset(a,0,sizeof(a)); memset(b,0,sizeof(b)); memset(c,0,sizeof(c)); memset(d,0,sizeof(d));
    a[0]=d[0]=1; memcpy(b,x,sizeof(b));
    for (int pos=254;pos>=0;pos--) {
        uint8_t bit=(uint8_t)((scalar_copy[pos>>3]>>(pos&7))&1); x25519_select(a,b,bit); x25519_select(c,d,bit);
        x25519_add(e,a,c); x25519_sub(a,a,c); x25519_add(c,b,d); x25519_sub(b,b,d);
        x25519_square(d,e); x25519_square(f,a); x25519_mul(a,c,a); x25519_mul(c,b,e);
        x25519_add(e,a,c); x25519_sub(a,a,c); x25519_square(b,e); x25519_square(c,a);
        x25519_sub(a,d,f); x25519_mul(a,a,a24); x25519_add(a,a,d); x25519_mul(c,c,a);
        x25519_mul(a,d,f); x25519_mul(d,b,x); x25519_square(b,e);
        x25519_select(a,b,bit); x25519_select(c,d,bit);
    }
    x25519_invert(c,c); x25519_mul(a,a,c); x25519_pack(out,a);
}

static bool read_record(tls13_client_t* c,uint8_t* type,uint8_t** payload,uint32_t* len,uint32_t timeout){
    uint8_t h[5];if(!recv_all(c->conn,h,5,timeout)){g_tls13_error="server hello timeout";return false;}uint16_t n=be16(h+3);if(n>TLS13_MAX_RECORD){g_tls13_error="record too large";return false;}
    if(!recv_all(c->conn,c->record,n,timeout)){g_tls13_error="server hello body timeout";return false;}*type=h[0];*payload=c->record;*len=n;return true;
}
static void nonce_for(const uint8_t iv[12],uint64_t seq,uint8_t out[12]){memcpy(out,iv,12);for(int i=0;i<8;i++)out[11-i]^=(uint8_t)(seq>>(i*8));}

static bool decrypt_record(tls13_client_t* c,const uint8_t key[16],const uint8_t iv[12],uint8_t* out,uint32_t* out_len,uint8_t* inner_type,uint32_t timeout){
    uint8_t type,*body;uint32_t n;if(!read_record(c,&type,&body,&n,timeout))return false;
    if(type==20&&n==1){*out_len=0;*inner_type=20;return true;}
    if(type!=23||n<17)return false;
    uint8_t hdr[5]={23,3,3,(uint8_t)(n>>8),(uint8_t)n},nonce[12];nonce_for(iv,c->rx_seq++,nonce);
    if(!kcrypto_aes128_gcm_decrypt_aad(key,nonce,hdr,5,body,n-16,body+n-16,out))return false;
    uint32_t p=n-16;while(p&&out[p-1]==0)p--;if(!p)return false;*out_len=p-1;*inner_type=out[p-1];return *inner_type==22||*inner_type==20||*inner_type==21||*inner_type==8||*inner_type==23;
}

static bool send_record(tls13_client_t* c,const uint8_t key[16],const uint8_t iv[12],const uint8_t* data,uint32_t len,uint8_t inner){
    if(len>16383)return false;uint8_t buf[16384+1+16+5],nonce[12],tag[16];uint32_t n=len+1+16;buf[0]=23;buf[1]=3;buf[2]=3;put16(buf+3,(uint16_t)n);memcpy(buf+5,data,len);buf[5+len]=inner;nonce_for(iv,c->tx_seq++,nonce);
    kcrypto_aes128_gcm_encrypt_aad(key,nonce,buf,5,buf+5,len+1,buf+5+len+1,tag);memcpy(buf+5+len+1+(len+1),tag,16);
    return send_all(c->conn,buf,5+n);
}

static bool make_client_hello(tls13_client_t* c,const char* host,uint8_t priv[32],uint8_t pub[32]){
    uint8_t ch[2048],random[32],sid[32];random_bytes(random,32);random_bytes(sid,32);random_bytes(priv,32);uint8_t base[32]={9};x25519(pub,priv,base);
    uint32_t p=0;put16(ch+p,0x0303);p+=2;memcpy(ch+p,random,32);p+=32;ch[p++]=32;memcpy(ch+p,sid,32);p+=32;
    put16(ch+p,2);p+=2;put16(ch+p,0x1301);p+=2;ch[p++]=1;ch[p++]=0;
    uint32_t ext_start=p;p+=2;uint32_t e=p;
    size_t hn=0;while(host[hn]&&hn<255)hn++;put16(ch+p,0);put16(ch+p+2,(uint16_t)(hn+5));put16(ch+p+4,(uint16_t)(hn+3));ch[p+6]=0;put16(ch+p+7,(uint16_t)hn);memcpy(ch+p+9,host,hn);p+=9+hn;
    put16(ch+p,0x002b);put16(ch+p+2,3);ch[p+4]=2;put16(ch+p+5,0x0304);p+=7;
    put16(ch+p,0x000a);put16(ch+p+2,4);put16(ch+p+4,2);put16(ch+p+6,0x001d);p+=8;
    /* key_share: client_shares<0..2^16-1> contains one X25519 entry. */
    put16(ch+p,0x0033);put16(ch+p+2,38);put16(ch+p+4,36);
    put16(ch+p+6,0x001d);put16(ch+p+8,32);memcpy(ch+p+10,pub,32);p+=42;
    put16(ch+p,0x000d);put16(ch+p+2,8);put16(ch+p+4,6);put16(ch+p+6,0x0403);put16(ch+p+8,0x0804);put16(ch+p+10,0x0503);p+=12;
    put16(ch+ext_start,(uint16_t)(p-e));uint32_t body=p;uint8_t hs[2048];hs[0]=1;put24(hs+1,body);memcpy(hs+4,ch,body);uint8_t rec[2053];rec[0]=22;rec[1]=3;rec[2]=1;put16(rec+3,(uint16_t)(body+4));memcpy(rec+5,hs,body+4);
    transcript_add(c,hs,body+4);return send_all(c->conn,rec,body+9);
}

static bool parse_server_hello(tls13_client_t* c,uint8_t peer_pub[32],uint8_t* server_random){
    uint8_t type,*p;uint32_t n;if(!read_record(c,&type,&p,&n,TLS13_TIMEOUT))return false;if(type==21){if(n>=2){g_tls13_error=tls_alert_name(p[1]);}else g_tls13_error="short server alert";return false;}if(type!=22){g_tls13_error="unexpected server record";return false;}if(n<4||p[0]!=2){g_tls13_error="not server hello";return false;}uint32_t m=be24(p+1);if(m+4>n||m<38){g_tls13_error="short server hello";return false;}static const uint8_t hrr_magic[8]={0xcf,0x21,0xad,0x74,0xe5,0x9a,0x61,0x11};if(kcrypto_memeq_ct(p+6,hrr_magic,8)){g_tls13_error="hello retry request";return false;}transcript_add(c,p,m+4);uint32_t q=4; q+=2;memcpy(server_random,p+q,32);q+=32;uint32_t sid=p[q++];q+=sid;if(q+3>m+4){g_tls13_error="server hello fields";return false;}uint16_t suite=be16(p+q);q+=2;if(suite!=0x1301){g_tls13_error="unsupported cipher";return false;}if(p[q++]!=0){g_tls13_error="server compression";return false;}uint16_t ex=be16(p+q);q+=2;uint32_t end=q+ex;bool have_key=false;while(q+4<=end){uint16_t id=be16(p+q),l=be16(p+q+2);q+=4;if(q+l>end){g_tls13_error="server extension length";return false;}if(id==0x0033&&l>=36&&be16(p+q)==0x001d&&be16(p+q+2)==32){memcpy(peer_pub,p+q+4,32);have_key=true;}q+=l;}if(q!=end){g_tls13_error="server extensions";return false;}if(!have_key){g_tls13_error="missing server key share";return false;}return true;
}

tls13_client_t* tls13_connect(uint32_t ip,uint16_t port,const char* host,uint32_t timeout_ms){
    g_tls13_error="tcp connect";tcp_conn_t* conn=tcp_connect(ip,port,timeout_ms?timeout_ms:5000);if(!conn)return NULL;tls13_client_t* c=(tls13_client_t*)alloc(sizeof(*c));if(!c){g_tls13_error="allocation";tcp_close(conn);return NULL;}memset(c,0,sizeof(*c));c->conn=conn;uint8_t priv[32],pub[32],peer[32],sr[32];
    g_tls13_error="client hello";if(!make_client_hello(c,host,priv,pub)){tcp_close(conn);free_mem(c);return NULL;}g_tls13_error="server hello";if(!parse_server_hello(c,peer,sr)){tcp_close(conn);free_mem(c);return NULL;}
    uint8_t shared[32],empty[32]={0},empty_hash[32],early[32],derived[32],hs_secret[32],th[32];x25519(shared,priv,peer);kcrypto_sha256(NULL,0,empty_hash);hkdf_extract(NULL,0,empty,32,early);hkdf_label(early,"derived",empty_hash,32,derived,32);hkdf_extract(derived,32,shared,32,hs_secret);transcript_hash(c,th);uint8_t shs[32],chs[32];hkdf_label(hs_secret,"s hs traffic",th,32,shs,32);hkdf_label(hs_secret,"c hs traffic",th,32,chs,32);hkdf_label(shs,"key",NULL,0,c->server_hs_key,16);hkdf_label(shs,"iv",NULL,0,c->server_hs_iv,12);hkdf_label(chs,"key",NULL,0,c->client_hs_key,16);hkdf_label(chs,"iv",NULL,0,c->client_hs_iv,12);
    bool finished=false;uint8_t plain[TLS13_MAX_RECORD];g_tls13_error="encrypted handshake";while(!finished){uint32_t plen;uint8_t inner;if(!decrypt_record(c,c->server_hs_key,c->server_hs_iv,plain,&plen,&inner,TLS13_TIMEOUT)){tcp_close(conn);free_mem(c);return NULL;}if(inner!=22)continue;uint32_t off=0;while(off+4<=plen){uint32_t ml=be24(plain+off+1);if(off+4+ml>plen)break;uint8_t mt=plain[off];if(mt==20){uint8_t fh[32],verify[32];transcript_hash(c,fh);uint8_t fk[32];hkdf_label(shs,"finished",NULL,0,fk,32);hmac_sha256(fk,32,fh,32,verify);if(ml!=32||!kcrypto_memeq_ct(verify,plain+off+4,32)){g_tls13_error="server finished verify";tcp_close(conn);free_mem(c);return NULL;}transcript_add(c,plain+off,ml+4);finished=true;}else transcript_add(c,plain+off,ml+4);off+=ml+4;}}
    transcript_hash(c,th);uint8_t master[32],d2[32],cap[32],sap[32];hkdf_label(hs_secret,"derived",empty_hash,32,d2,32);hkdf_extract(d2,32,empty,32,master);hkdf_label(master,"c ap traffic",th,32,cap,32);hkdf_label(master,"s ap traffic",th,32,sap,32);hkdf_label(cap,"key",NULL,0,c->client_app_key,16);hkdf_label(cap,"iv",NULL,0,c->client_app_iv,12);hkdf_label(sap,"key",NULL,0,c->server_app_key,16);hkdf_label(sap,"iv",NULL,0,c->server_app_iv,12);
    uint8_t fh[32],fk[32],fin[32],msg[36];transcript_hash(c,fh);hkdf_label(chs,"finished",NULL,0,fk,32);hmac_sha256(fk,32,fh,32,fin);msg[0]=20;put24(msg+1,32);memcpy(msg+4,fin,32);g_tls13_error="client finished";if(!send_record(c,c->client_hs_key,c->client_hs_iv,msg,36,22)){tcp_close(conn);free_mem(c);return NULL;}transcript_add(c,msg,36);c->tx_seq=0;c->rx_seq=0;c->established=true;serial_write_str("tls13: connected (certificate verification deferred)\n");return c;
}

int32_t tls13_send(tls13_client_t* c,const void* data,uint32_t len){if(!c||!c->established||len>16383)return -1;return send_record(c,c->client_app_key,c->client_app_iv,(const uint8_t*)data,len,23)?(int32_t)len:-1;}
int32_t tls13_recv(tls13_client_t* c,void* data,uint32_t cap,uint32_t timeout){if(!c||!c->established)return -1;uint8_t plain[TLS13_MAX_RECORD];for(;;){uint32_t n;uint8_t inner;if(!decrypt_record(c,c->server_app_key,c->server_app_iv,plain,&n,&inner,timeout))return -1;if(inner!=23){if(inner==21)return -1;continue;}if(n>cap)return -1;memcpy(data,plain,n);return (int32_t)n;}}
void tls13_close(tls13_client_t* c){if(!c)return;if(c->conn)tcp_close(c->conn);memset(c,0,sizeof(*c));free_mem(c);}
