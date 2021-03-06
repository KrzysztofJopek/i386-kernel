#include "proc.h"
#include "halloc.h"
#include "strutils.h"
#include "log.h"
#include <stdint.h>
#include "vm.h"
#include "mm.h"
#include "syscall.h"

struct context{
	uint32_t edi;
	uint32_t esi;
	uint32_t ebx;
	uint32_t ebp;
	uint32_t eip;
};


#define MAX_PROC 10
static struct proc procs[MAX_PROC];
static struct proc sched_proc;

static int32_t find_free_proc()
{
	for(uint32_t i=0; i<MAX_PROC; i++){
		if(!procs[i].used){
			return i;
		}
	}
	return -1;
}

void swtch(struct context** old, struct context* new);
void setup_ctx(struct context* ctx);
void trapret();

/*
 * First executed function on new process, returns to trapret
 */
void forkret()
{
	return;
}

static int32_t create_process()
{
	int pid = find_free_proc();
	if(pid == -1){
		LOG_WRN("no free procs");
		return -1;
	}
	procs[pid].used = 1;
	void* frame = halloc(PAGE_SIZE);
	if(frame == 0){
		LOG_WRN("oom");
		return -1;
	}
	procs[pid].kstack = frame;
	void* sp = procs[pid].kstack + PAGE_SIZE;
	sp -= sizeof(struct trapframe);
	procs[pid].tf = (struct trapframe*)sp;
	sp -= 4;
	*(uint32_t*)sp = (uint32_t)trapret;
	sp -= sizeof(struct context);
	procs[pid].ctx = sp;
	setup_ctx(procs[pid].ctx);
	procs[pid].ctx->eip = (uint32_t)forkret;
	return pid;
}

/*
 * Starts first user process
 */
static void setup_init_proc()
{
	uint32_t pid = create_process();
	procs[pid].pgdir = setup_user();
	memset(procs[pid].tf, 0, sizeof(struct trapframe));

	procs[pid].tf->cs = 0x1B;
	procs[pid].tf->ds = 0x23;
	procs[pid].tf->es = 0x23;
	procs[pid].tf->ss = 0x23;
	procs[pid].tf->eflags = 0x00000200;
	procs[pid].tf->esp = PAGE_SIZE;
	procs[pid].tf->eip = 0x10;
	procs[pid].start = 1;
	procs[pid].pid = pid;
}

//from osdev.org
struct tss_entry_struct
{
	int32_t prev_tss;   // The previous TSS - if we used hardware task switching this would form a linked list.
	int32_t esp0;       // The stack pointer to load when we change to kernel mode.
	int32_t ss0;        // The stack segment to load when we change to kernel mode.
	int32_t esp1;       // everything below here is unusued now..
	int32_t ss1;
	int32_t esp2;
	int32_t ss2;
	int32_t cr3;
	int32_t eip;
	int32_t eflags;
	int32_t eax;
	int32_t ecx;
	int32_t edx;
	int32_t ebx;
	int32_t esp;
	int32_t ebp;
	int32_t esi;
	int32_t edi;
	int32_t es;         
	int32_t cs;        
	int32_t ss;        
	int32_t ds;        
	int32_t fs;       
	int32_t gs;         
	int32_t ldt;      
	int16_t trap;
	int16_t iomap_base;
} __attribute__((packed));

struct gdt_entry_bits
{
	unsigned int limit_low:16;
	unsigned int base_low : 24;
	//attribute byte split into bitfields
	unsigned int accessed :1;
	unsigned int read_write :1; //readable for code, writable for data
	unsigned int conforming_expand_down :1; //conforming for code, expand down for data
	unsigned int code :1; //1 for code, 0 for data
	unsigned int always_1 :1; //should be 1 for everything but TSS and LDT
	unsigned int DPL :2; //priviledge level
	unsigned int present :1;
	//and now into granularity
	unsigned int limit_high :4;
	unsigned int available :1;
	unsigned int always_0 :1; //should always be 0
	unsigned int big :1; //32bit opcodes for code, uint32_t stack for data
	unsigned int gran :1; //1 to use 4k page addressing, 0 for byte addressing
	unsigned int base_high :8;
} __attribute__((packed));

static struct tss_entry_struct tss_entry;
static void write_tss(struct gdt_entry_bits *g)
{
	// Firstly, let's compute the base and limit of our entry into the GDT.
	uint32_t base = (uint32_t) &tss_entry;
	uint32_t limit = sizeof(tss_entry);
	
	// Now, add our TSS descriptor's address to the GDT.
	g->limit_low=limit&0xFFFF;
	g->base_low=base&0xFFFFFF; //isolate bottom 24 bits
	g->accessed=1; //This indicates it's a TSS and not a LDT. This is a changed meaning
	g->read_write=0; //This indicates if the TSS is busy or not. 0 for not busy
	g->conforming_expand_down=0; //always 0 for TSS
	g->code=1; //For TSS this is 1 for 32bit usage, or 0 for 16bit.
	g->always_1=0; //indicate it is a TSS
	g->DPL=3; //same meaning
	g->present=1; //same meaning
	g->limit_high=(limit&0xF0000)>>16; //isolate top nibble
	g->available=0;
	g->always_0=0; //same thing
	g->big=0; //should leave zero according to manuals. No effect
	g->gran=0; //so that our computed GDT limit is in bytes, not pages
	g->base_high=(base&0xFF000000)>>24; //isolate top byte.
	
	// Ensure the TSS is initially zero'd.
	memset(&tss_entry, 0, sizeof(tss_entry));
	
	tss_entry.ss0  = 0x10;  // Set the kernel stack segment.
	tss_entry.esp0 = 0x0; // Set the kernel stack pointer.
	//note that CS is loaded from the IDT entry and should be the regular kernel code segment
}

void gdt_tss();
static void set_tss()
{
	void* tss_addr = (void*)gdt_tss;
	write_tss(tss_addr);
	asm volatile(
	"mov ax, 0x2b;"
	"ltr ax;");
}

static void set_active_proc(uint32_t pid)
{
	currproc = procs+pid;
}

static void fork()
{
	uint32_t pid = create_process();
	procs[pid].pgdir = setup_user();
	copy_user_space(currproc->pgdir, procs[pid].pgdir);
	memcpy(procs[pid].tf, currproc->tf, sizeof(struct trapframe));
	currproc->tf->eax = pid;
	procs[pid].tf->eax = 0;
	procs[pid].start = 1;
	procs[pid].pid = pid;
}

void scheduler()
{
	add_syscall(5, fork, 0);
	//init scheduler
	set_tss();
	sched_proc.ctx = halloc(sizeof(struct context));
	if(!sched_proc.ctx){
		LOG_ERR("Cant allocate sched context");
	}
	setup_init_proc();

	for(;;){
		for(int p = 0; p<MAX_PROC; p++){
			if(procs[p].start == 0){
				continue;
			}
			set_active_proc(p);
			tss_entry.esp0 = (uint32_t)(procs[p].kstack + PAGE_SIZE);
			set_upgdir(procs[p].pgdir);
			swtch(&sched_proc.ctx, procs[p].ctx);
			set_kpgdir();
		}
	};
}


void sched()
{
	swtch(&currproc->ctx, sched_proc.ctx);
}

void keyboard_handler_main();
void uart_handler_main();
void trap(struct trapframe *tf)
{
	if(tf->trapno == 0x21){
		keyboard_handler_main();
	}
	else if(tf->trapno == 0x24){
		uart_handler_main();
	}
	else if(tf->trapno == 0x80){
		LOG("Syscall %d, pid:%d", tf->eax, currproc->pid);
		tf->eax = call_syscall(tf);
	}
	sched();
	return;
}

