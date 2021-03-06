#include <stdio.h>
#include <strings.h>

#include "lwip/opt.h"
#include "lwip/debug.h"
#include "lwip/stats.h"
#include "lwip/tcp.h"
#include "lwip/mem.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/netif.h"
#include "lwip/sys.h"
#include "lwip/timers.h"
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "netif/etharp.h"

#include "fsl_clock_manager.h"
#include "fsl_os_abstraction.h"
#include "fsl_interrupt_manager.h"
#include "ethernetif.h"
#include "board.h"

#include "queue.h"

#define TCP_PORT	23

enum telnet_states
{
    ES_NONE = 0,
    ES_ACCEPTED,
    ES_RECEIVED,
    ES_CLOSING
};

struct telnet_state
{
    u8_t state;
    u8_t retries;
    struct tcp_pcb *pcb;
    // pbuf (chain) to recycle
    struct pbuf *p;
};

err_t telnet_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
err_t telnet_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
void telnet_error(void *arg, err_t err);
err_t telnet_poll(void *arg, struct tcp_pcb *tpcb);
err_t telnet_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
void telnet_send(struct tcp_pcb *tpcb, struct telnet_state *es);
void telnet_close(struct tcp_pcb *tpcb, struct telnet_state *es);

static struct tcp_pcb *telnet_pcb;
static queue_t fifo;

void led_init(void)
{
    GPIO_DRV_Init(NULL, ledPins);

    LED1_EN;
    LED2_EN;
    LED3_EN;
}

void led_off(void)
{
	LED2_OFF;
	LED1_OFF;
	LED3_OFF;
}

void led_red(void)
{
	led_off();
	LED2_ON;
}

void led_green(void)
{
	led_off();
	LED1_ON;
}

void led_blue(void)
{
	led_off();
	LED3_ON;
}

void led_all(void)
{
	LED2_ON;
	LED1_ON;
	LED3_ON;
}

void telnet_init(void)
{
    telnet_pcb = tcp_new();
    if (telnet_pcb != NULL)
    {
        err_t err;

        err = tcp_bind(telnet_pcb, IP_ADDR_ANY, TCP_PORT);
        if (err == ERR_OK)
        {
            telnet_pcb = tcp_listen(telnet_pcb);
            tcp_accept(telnet_pcb, telnet_accept);
        }
        else 
        {
            // abort? output diagnostic?
        }
    }
    else
    {
        // abort? output diagnostic?
    }
}

err_t telnet_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    err_t ret_err;
    struct telnet_state *es;

    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(err);

    // Unless this pcb should have NORMAL priority, set its priority now.
    // When running out of pcbs, low priority pcbs can be aborted to create
    // new pcbs of higher priority.
    tcp_setprio(newpcb, TCP_PRIO_MIN);

    es = (struct telnet_state *)mem_malloc(sizeof(struct telnet_state));
    if (es != NULL)
    {
        es->state = ES_ACCEPTED;
        es->pcb = newpcb;
        es->retries = 0;
        es->p = NULL;
        // pass newly allocated es to our callbacks
        tcp_arg(newpcb, es);
        tcp_recv(newpcb, telnet_recv);
        tcp_err(newpcb, telnet_error);
        tcp_poll(newpcb, telnet_poll, 0);
        ret_err = ERR_OK;
    }
    else
    {
        ret_err = ERR_MEM;
    }
    return ret_err;  
}

err_t telnet_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    struct telnet_state *es;
    err_t ret_err;

    LWIP_ASSERT("arg != NULL",arg != NULL);
    es = (struct telnet_state *)arg;
    if (p == NULL)
    {
        // remote host closed connection 
        es->state = ES_CLOSING;
        if(es->p == NULL)
        {
            // we're done sending, close it
            telnet_close(tpcb, es);
        }
        else
        {
            // we're not done yet
            tcp_sent(tpcb, telnet_sent);
            telnet_send(tpcb, es);
        }
        ret_err = ERR_OK;
    }
    else if(err != ERR_OK)
    {
        // cleanup, for unkown reason
        if (p != NULL)
        {
            es->p = NULL;
            pbuf_free(p);
        }
        ret_err = err;
    }
    else if(es->state == ES_ACCEPTED)
    {
        // first data chunk in p->payload
        es->state = ES_RECEIVED;
        // store reference to incoming pbuf (chain)
        es->p = p;
        // install send completion notifier
        tcp_sent(tpcb, telnet_sent);
        telnet_send(tpcb, es);
        ret_err = ERR_OK;
    }
    else if (es->state == ES_RECEIVED)
    {
        // read some more data
        if(es->p == NULL)
        {
            es->p = p;
            tcp_sent(tpcb, telnet_sent);
            telnet_send(tpcb, es);
        }
        else
        {
            struct pbuf *ptr;

            // chain pbufs to the end of what we recv'ed previously
            ptr = es->p;
            pbuf_chain(ptr,p);
        }
        ret_err = ERR_OK;
    }
    else if(es->state == ES_CLOSING)
    {
        // odd case, remote side closing twice, trash data
        tcp_recved(tpcb, p->tot_len);
        es->p = NULL;
        pbuf_free(p);
        ret_err = ERR_OK;
    }
    else
    {
        // unkown es->state, trash data
        tcp_recved(tpcb, p->tot_len);
        es->p = NULL;
        pbuf_free(p);
        ret_err = ERR_OK;
    }
    return ret_err;
}

void telnet_error(void *arg, err_t err)
{
    struct telnet_state *es;

    LWIP_UNUSED_ARG(err);

    es = (struct telnet_state *)arg;
    if (es != NULL)
    {
        mem_free(es);
    }
}

err_t telnet_poll(void *arg, struct tcp_pcb *tpcb)
{
    err_t ret_err;
    struct telnet_state *es;

    es = (struct telnet_state *)arg;
    if (es != NULL)
    {
        if (es->p != NULL)
        {
            // there is a remaining pbuf (chain)
            tcp_sent(tpcb, telnet_sent);
            telnet_send(tpcb, es);
        }
        else
        {
            // no remaining pbuf (chain)
            if(es->state == ES_CLOSING)
            {
            telnet_close(tpcb, es);
            }
        }
        ret_err = ERR_OK;
    }
    else
    {
        // nothing to be done
        tcp_abort(tpcb);
        ret_err = ERR_ABRT;
    }
    return ret_err;
}

err_t telnet_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    struct telnet_state *es;

    LWIP_UNUSED_ARG(len);

    es = (struct telnet_state *)arg;
    es->retries = 0;

    if(es->p != NULL)
    {
        // still got pbufs to send
        tcp_sent(tpcb, telnet_sent);
        telnet_send(tpcb, es);
    }
    else
    {
        // no more pbufs to send
        if(es->state == ES_CLOSING)
        {
            telnet_close(tpcb, es);
        }
    }
    return ERR_OK;
}

void telnet_send(struct tcp_pcb *tpcb, struct telnet_state *es)
{
    struct pbuf *ptr;
    err_t wr_err = ERR_OK;

    while ((wr_err == ERR_OK) &&
         (es->p != NULL) && 
         (es->p->len <= tcp_sndbuf(tpcb)))
    {
        ptr = es->p;

        // enqueue data for transmissionptr->payload
        wr_err = tcp_write(tpcb, ptr->payload, ptr->len, 1);

        {
        	int i;
        	char *data = (char *) ptr->payload;

        	for (i = 0; i < ptr->len-1;i++)
        		queue_push(fifo, data[i]);
        }

        if (wr_err == ERR_OK)
        {
            u16_t plen;
            u8_t freed;

            plen = ptr->len;
            // continue with next pbuf in chain (if any)
            es->p = ptr->next;
            if(es->p != NULL)
            {
                // new reference!
                pbuf_ref(es->p);
            }
            // chop first pbuf from chain
            do
            {
                // try hard to free pbuf
                freed = pbuf_free(ptr);
            }
            while(freed == 0);
            // we can read more data now
            tcp_recved(tpcb, plen);
        }
        else if(wr_err == ERR_MEM)
        {
            // we are low on memory, try later / harder, defer to poll
            es->p = ptr;
        }
        else
        {
            // other problem ?? 
        }
    }
}

void telnet_close(struct tcp_pcb *tpcb, struct telnet_state *es)
{
    tcp_arg(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_err(tpcb, NULL);
    tcp_poll(tpcb, NULL, 0);

    if (es != NULL)
    {
        mem_free(es);
    }  
    tcp_close(tpcb);
}

static void app_low_level_init(void)
{
    hardware_init();
    CLOCK_SYS_EnableEnetClock(0);
    CLOCK_SYS_SetEnetTimeStampSrc(0, kClockTimeSrcOsc0erClk);
    MPU_BWR_CESR_VLD(MPU, 0);
}

void dump(const char *label, uint8_t *buffer, size_t size)
{
	int i;

	printf("%s: ", label);
	for (i = 0; i < size; i++)
		printf("0x%02X ", buffer[i]);

	putchar('\n');
}

static char command[64];

int command_read(void)
{
	static int index = 0;
	int data, ret = 0;

	INT_SYS_DisableIRQGlobal();
	while (queue_pop(fifo, &data) != -1)
	{
		if (data >= ' ' && data <= '~') {
			command[index++] = data;
		}
		else {
			command[index] = '\0';
			index = 0;
			ret = 1;
		}
	}
	INT_SYS_EnableIRQGlobal();

	return ret;
}

int main_loop(void)
{

	while(1)
    {
		if (command_read()) {
#if 1
			printf("cmd: %s\n", command);
			if (!strcasecmp(command, "red"))
				led_red();
			else if (!strcasecmp(command, "green"))
				led_green();
			else if (!strcasecmp(command, "blue"))
				led_blue();
			else if (!strcasecmp(command, "white"))
				led_all();
			else if (!strcasecmp(command, "none"))
				led_off();
#endif
		}
        sys_check_timeouts();
    }
}

int main(void)
{
    struct netif fsl_netif0;
    ip_addr_t fsl_netif0_ipaddr, fsl_netif0_netmask, fsl_netif0_gw;

    app_low_level_init();
    OSA_Init();
    led_init();

    printf("\r\nheynet\r\n");

    lwip_init();

    fifo = queue_init();
	
    IP4_ADDR(&fsl_netif0_ipaddr, 192,168,1,100);
    IP4_ADDR(&fsl_netif0_netmask, 255,255,255,0);
    IP4_ADDR(&fsl_netif0_gw, 192,168,2,100);
    netif_add(&fsl_netif0,
    		  &fsl_netif0_ipaddr,
			  &fsl_netif0_netmask,
			  &fsl_netif0_gw,
			  NULL,
			  ethernetif_init,
			  ethernet_input);
    netif_set_default(&fsl_netif0);
    netif_set_up(&fsl_netif0);
    telnet_init();

    return main_loop();
}
