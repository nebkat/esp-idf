/*
 * FreeRTOS Kernel V10.5.1 (ESP-IDF SMP modified)
 * Copyright (C) 2020 Cambridge Consultants Ltd.
 *
 * SPDX-FileCopyrightText: 2020 Cambridge Consultants Ltd
 *
 * SPDX-License-Identifier: MIT
 *
 * SPDX-FileContributor: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

/*-----------------------------------------------------------
 * Implementation of functions defined in portable.h for the Posix port.
 *
 * Each task has a pthread which eases use of standard debuggers
 * (allowing backtraces of tasks etc). Threads for tasks that are not
 * running are blocked in sigwait().
 *
 * Task switch is done by resuming the thread for the next task by
 * signaling the condition variable and then waiting on a condition variable
 * with the current thread.
 *
 * The timer interrupt uses SIGALRM and care is taken to ensure that
 * the signal handler runs only on the thread for the current task.
 *
 * Simulated interrupts (vPortGenerateSimulatedInterrupt()) let code outside
 * FreeRTOS, such as a thread waiting on host I/O, interrupt the scheduler the
 * way a peripheral does. An interrupt is a bit in a pending register; raising it
 * sends SIG_INTERRUPT to the thread of the running task, which takes it as an ISR
 * once it has interrupts enabled. A critical section defers it to its end, as on
 * a CPU: re-enabling interrupts, and the end of an ISR, raise SIG_INTERRUPT again
 * while bits are pending, so one aimed at a task that switched out meanwhile is
 * taken by the next.
 *
 * A task inside the host's C library may hold one of its locks (a pthread mutex or
 * os_unfair_lock, which FreeRTOS cannot see), and switching it out there would strand
 * the lock: a task waiting on it looks runnable, so the holder is never scheduled again.
 * vPortDeferSwitches() and vPortAllowSwitches() bracket such a call; between them the
 * tick and interrupts still run, but a switch they would make waits for the end.
 *
 * Use of part of the standard C library requires care as some
 * functions can take pthread mutexes internally which can result in
 * deadlocks as the FreeRTOS kernel can switch tasks while they're
 * holding a pthread mutex.
 *
 * stdio (printf() and friends) should be called from a single task
 * only or serialized with a FreeRTOS primitive such as a binary
 * semaphore or mutex.
 *----------------------------------------------------------*/

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "utils/wait_for_event.h"
/*-----------------------------------------------------------*/

#define SIG_RESUME SIGUSR1
#define SIG_INTERRUPT SIGUSR2

typedef struct THREAD
{
    pthread_t pthread;
    TaskFunction_t pxCode;
    void *pvParams;
    BaseType_t xDying;
    struct event *ev;
    /* vPortDeferSwitches() nesting, and whether a switch waits for it to end. */
    volatile BaseType_t uxSwitchDeferrals;
    volatile BaseType_t xSwitchDeferred;
} Thread_t;

/*
 * The additional per-thread data is stored at the beginning of the
 * task's stack.
 */
static inline Thread_t *prvGetThreadFromTask(TaskHandle_t xTask)
{
    StackType_t *pxTopOfStack = *(StackType_t **)xTask;

    return (Thread_t *)(pxTopOfStack + 1);
}

/*-----------------------------------------------------------*/

static pthread_once_t hSigSetupThread = PTHREAD_ONCE_INIT;
static sigset_t xAllSignals;
static sigset_t xSchedulerOriginalSignalMask;
static pthread_t hMainThread = ( pthread_t )NULL;
static volatile BaseType_t uxCriticalNesting;
/*-----------------------------------------------------------*/

static uint32_t ( *pvInterruptHandlers[ portMAX_INTERRUPTS ] )( void );
static uint32_t ulPendingInterrupts;
/* The thread of the running task: the one interrupts are aimed at. */
static Thread_t *pxRunningThread;
/*-----------------------------------------------------------*/

static BaseType_t xSchedulerEnd = pdFALSE;
/*-----------------------------------------------------------*/

static void prvSetupSignalsAndSchedulerPolicy( void );
static void prvSetupTimerInterrupt( void );
static void *prvWaitForStart( void * pvParams );
static void prvSwitchThread( Thread_t * xThreadToResume,
                             Thread_t *xThreadToSuspend );
static void prvSuspendSelf( Thread_t * thread);
static void prvResumeThread( Thread_t * xThreadId );
static void vPortSystemTickHandler( int sig );
static void prvInterruptHandler( int sig );
static void prvRaisePendingInterrupts( void );
static void vPortStartFirstTask( void );
/*-----------------------------------------------------------*/

static void prvFatalError( const char *pcCall, int iErrno )
{
    fprintf( stderr, "%s: %s\n", pcCall, strerror( iErrno ) );
    abort();
}

/*
 * See header file for description.
 */
StackType_t *pxPortInitialiseStack( StackType_t *pxTopOfStack,
                                    StackType_t *pxEndOfStack,
                                    TaskFunction_t pxCode,
                                    void *pvParameters )
{
    Thread_t *thread;
    pthread_attr_t xThreadAttributes;
    size_t ulStackSize;
    int iRet;

    (void)pthread_once( &hSigSetupThread, prvSetupSignalsAndSchedulerPolicy );

    /*
     * Store the additional thread data at the start of the stack.
     */
    thread = (Thread_t *)(pxTopOfStack + 1) - 1;
    pxTopOfStack = (StackType_t *)thread - 1;
    ulStackSize = (pxTopOfStack + 1 - pxEndOfStack) * sizeof(*pxTopOfStack);

    thread->pxCode = pxCode;
    thread->pvParams = pvParameters;
    thread->xDying = pdFALSE;
    thread->uxSwitchDeferrals = 0;
    thread->xSwitchDeferred = pdFALSE;

    pthread_attr_init( &xThreadAttributes );
    pthread_attr_setstack( &xThreadAttributes, pxEndOfStack, ulStackSize );

    thread->ev = event_create();

    vPortEnterCritical();

    iRet = pthread_create( &thread->pthread, &xThreadAttributes,
                           prvWaitForStart, thread );
    if ( iRet )
    {
        prvFatalError( "pthread_create", iRet );
    }

    vPortExitCritical();

    return pxTopOfStack;
}
/*-----------------------------------------------------------*/

void vPortStartFirstTask( void )
{
    Thread_t *pxFirstThread = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );

    /* Start the first task. */
    __atomic_store_n( &pxRunningThread, pxFirstThread, __ATOMIC_SEQ_CST );
    prvResumeThread( pxFirstThread );
}
/*-----------------------------------------------------------*/

/*
 * See header file for description.
 */
BaseType_t xPortStartScheduler( void )
{
    int iSignal;
    sigset_t xSignals;

    hMainThread = pthread_self();

    /* Start the timer that generates the tick ISR(SIGALRM).
       Interrupts are disabled here already. */
    prvSetupTimerInterrupt();

    /* Start the first task. */
    vPortStartFirstTask();

    /* Wait until signaled by vPortEndScheduler(). */
    sigemptyset( &xSignals );
    sigaddset( &xSignals, SIG_RESUME );

    while ( !xSchedulerEnd )
    {
        sigwait( &xSignals, &iSignal );
    }

    /* Cancel the Idle task and free its resources */
#if ( INCLUDE_xTaskGetIdleTaskHandle == 1 )
    vPortCancelThread( xTaskGetIdleTaskHandle() );
#endif

#if ( configUSE_TIMERS == 1 )
    /* Cancel the Timer task and free its resources */
    vPortCancelThread( xTimerGetTimerDaemonTaskHandle() );
#endif /* configUSE_TIMERS */

    /* Restore original signal mask. */
    (void)pthread_sigmask( SIG_SETMASK, &xSchedulerOriginalSignalMask,  NULL );

    return 0;
}
/*-----------------------------------------------------------*/

void vPortEndScheduler( void )
{
    struct itimerval itimer;
    struct sigaction sigtick;
    Thread_t *xCurrentThread;

    /* Stop the timer and ignore any pending SIGALRMs that would end
     * up running on the main thread when it is resumed. */
    itimer.it_value.tv_sec = 0;
    itimer.it_value.tv_usec = 0;

    itimer.it_interval.tv_sec = 0;
    itimer.it_interval.tv_usec = 0;
    (void)setitimer( ITIMER_REAL, &itimer, NULL );

    sigtick.sa_flags = 0;
    sigtick.sa_handler = SIG_IGN;
    sigemptyset( &sigtick.sa_mask );
    sigaction( SIGALRM, &sigtick, NULL );
    sigaction( SIG_INTERRUPT, &sigtick, NULL );

    /* Signal the scheduler to exit its loop. */
    xSchedulerEnd = pdTRUE;
    (void)pthread_kill( hMainThread, SIG_RESUME );

    xCurrentThread = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );
    prvSuspendSelf(xCurrentThread);
}
/*-----------------------------------------------------------*/

void vPortEnterCritical( void )
{
    if ( uxCriticalNesting == 0 )
    {
        vPortDisableInterrupts();
    }
    uxCriticalNesting++;
}
/*-----------------------------------------------------------*/

void vPortExitCritical( void )
{
    if ( uxCriticalNesting > 0 )
    {
        uxCriticalNesting--;
    }

    /* Critical section nesting count must always be >= 0. */
    configASSERT( uxCriticalNesting >= 0 );

    /* If we have reached 0 then re-enable the interrupts. */
    if( uxCriticalNesting == 0 )
    {
        vPortEnableInterrupts();
    }
}
/*-----------------------------------------------------------*/

void vPortYieldFromISR( void )
{
    Thread_t *xThreadToSuspend;
    Thread_t *xThreadToResume;

    xThreadToSuspend = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );

    vTaskSwitchContext();

    xThreadToResume = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );

    prvSwitchThread( xThreadToResume, xThreadToSuspend );
}
/*-----------------------------------------------------------*/

void vPortYield( void )
{
    vPortEnterCritical();

    vPortYieldFromISR();

    vPortExitCritical();
}
/*-----------------------------------------------------------*/

void vPortDisableInterrupts( void )
{
    pthread_sigmask( SIG_BLOCK, &xAllSignals, NULL );
}
/*-----------------------------------------------------------*/

void vPortEnableInterrupts( void )
{
    pthread_sigmask( SIG_UNBLOCK, &xAllSignals, NULL );
    /* Taken here, now that interrupts are enabled, if one is still pending. */
    prvRaisePendingInterrupts();
}
/*-----------------------------------------------------------*/

void vPortSetInterruptHandler( uint32_t ulInterruptNumber, uint32_t ( *pvHandler )( void ) )
{
    configASSERT( ulInterruptNumber < portMAX_INTERRUPTS );
    __atomic_store_n( &pvInterruptHandlers[ ulInterruptNumber ], pvHandler, __ATOMIC_SEQ_CST );
}
/*-----------------------------------------------------------*/

void vPortGenerateSimulatedInterrupt( uint32_t ulInterruptNumber )
{
    Thread_t *pxThread;

    configASSERT( ulInterruptNumber < portMAX_INTERRUPTS );
    __atomic_fetch_or( &ulPendingInterrupts, 1UL << ulInterruptNumber, __ATOMIC_SEQ_CST );

    /* Before the scheduler starts, the first task takes it as it enables
     * interrupts. */
    pxThread = __atomic_load_n( &pxRunningThread, __ATOMIC_SEQ_CST );
    if ( pxThread != NULL )
    {
        (void)pthread_kill( pxThread->pthread, SIG_INTERRUPT );
    }
}
/*-----------------------------------------------------------*/

/* The running task's thread, if this is it. */
static Thread_t *prvThisRunningThread( void )
{
    Thread_t *pxThread = __atomic_load_n( &pxRunningThread, __ATOMIC_SEQ_CST );

    return ( pxThread != NULL && pthread_equal( pxThread->pthread, pthread_self() ) ) ? pxThread : NULL;
}
/*-----------------------------------------------------------*/

void vPortDeferSwitches( void )
{
    Thread_t *pxThread = prvThisRunningThread();

    if ( pxThread != NULL )
    {
        pxThread->uxSwitchDeferrals++;
    }
}
/*-----------------------------------------------------------*/

void vPortAllowSwitches( void )
{
    Thread_t *pxThread = prvThisRunningThread();

    if ( pxThread == NULL || pxThread->uxSwitchDeferrals == 0 )
    {
        return;
    }
    if ( --pxThread->uxSwitchDeferrals == 0 && pxThread->xSwitchDeferred != pdFALSE )
    {
        pxThread->xSwitchDeferred = pdFALSE;
        /* Inside a critical section the next tick makes it instead. */
        if ( uxCriticalNesting == 0 )
        {
            vPortYield();
        }
    }
}
/*-----------------------------------------------------------*/

/* Only on the running task's thread: the main thread enables interrupts too, around
 * creating tasks before the scheduler starts. */
static void prvRaisePendingInterrupts( void )
{
    Thread_t *pxThread = __atomic_load_n( &pxRunningThread, __ATOMIC_SEQ_CST );

    if ( pxThread != NULL && pthread_equal( pxThread->pthread, pthread_self() ) &&
         __atomic_load_n( &ulPendingInterrupts, __ATOMIC_SEQ_CST ) != 0 )
    {
        (void)pthread_kill( pthread_self(), SIG_INTERRUPT );
    }
}
/*-----------------------------------------------------------*/

static void prvInterruptHandler( int sig )
{
    Thread_t *pxThreadToSuspend;
    Thread_t *pxThreadToResume;
    uint32_t ulPending;
    uint32_t ulSwitchRequired = pdFALSE;
    int iSavedErrno = errno;

    (void)sig;
    uxCriticalNesting++; /* Signals are blocked in this signal handler. */

    ulPending = __atomic_exchange_n( &ulPendingInterrupts, 0, __ATOMIC_SEQ_CST );
    for ( uint32_t ulInterrupt = 0; ulPending != 0; ulInterrupt++, ulPending >>= 1 )
    {
        uint32_t ( *pvHandler )( void ) = __atomic_load_n( &pvInterruptHandlers[ ulInterrupt ], __ATOMIC_SEQ_CST );
        if ( ( ulPending & 1 ) != 0 && pvHandler != NULL )
        {
            ulSwitchRequired |= pvHandler();
        }
    }

    if ( ulSwitchRequired != pdFALSE )
    {
        pxThreadToSuspend = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );
        if ( pxThreadToSuspend->uxSwitchDeferrals != 0 )
        {
            pxThreadToSuspend->xSwitchDeferred = pdTRUE;
        }
        else
        {
            vTaskSwitchContext();
            pxThreadToResume = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );
            prvSwitchThread( pxThreadToResume, pxThreadToSuspend );
        }
    }

    /* Raised while this ran, or aimed at a thread that has since switched out:
     * taken once this returns. */
    prvRaisePendingInterrupts();

    uxCriticalNesting--;
    errno = iSavedErrno;
}
/*-----------------------------------------------------------*/

BaseType_t xPortSetInterruptMask( void )
{
    /* Interrupts are always disabled inside ISRs (signals
       handlers). */
    return pdTRUE;
}
/*-----------------------------------------------------------*/

void vPortClearInterruptMask( BaseType_t xMask )
{
}
/*-----------------------------------------------------------*/

static uint64_t prvGetTimeNs(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

static uint64_t prvStartTimeNs;
/* commented as part of the code below in vPortSystemTickHandler,
 * to adjust timing according to full demo requirements */
/* static uint64_t prvTickCount; */

/*
 * Setup the systick timer to generate the tick interrupts at the required
 * frequency.
 */
void prvSetupTimerInterrupt( void )
{
    struct itimerval itimer;
    int iRet;

    /* Initialise the structure with the current timer information. */
    iRet = getitimer( ITIMER_REAL, &itimer );
    if ( iRet )
    {
        prvFatalError( "getitimer", errno );
    }

    /* Set the interval between timer events. */
    itimer.it_interval.tv_sec = 0;
    itimer.it_interval.tv_usec = portTICK_RATE_MICROSECONDS;

    /* Set the current count-down. */
    itimer.it_value.tv_sec = 0;
    itimer.it_value.tv_usec = portTICK_RATE_MICROSECONDS;

    /* Set-up the timer interrupt. */
    iRet = setitimer( ITIMER_REAL, &itimer, NULL );
    if ( iRet )
    {
        prvFatalError( "setitimer", errno );
    }

    prvStartTimeNs = prvGetTimeNs();
}
/*-----------------------------------------------------------*/

static void vPortSystemTickHandler( int sig )
{
    Thread_t *pxThreadToSuspend;
    Thread_t *pxThreadToResume;
    /* uint64_t xExpectedTicks; */

    uxCriticalNesting++; /* Signals are blocked in this signal handler. */

#if ( configUSE_PREEMPTION == 1 )
    pxThreadToSuspend = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );
#endif

    /* Tick Increment, accounting for any lost signals or drift in
     * the timer. */
/*
 *      Comment code to adjust timing according to full demo requirements
 *      xExpectedTicks = (prvGetTimeNs() - prvStartTimeNs)
 *        / (portTICK_RATE_MICROSECONDS * 1000);
 * do { */
        xTaskIncrementTick();
/*        prvTickCount++;
 *    } while (prvTickCount < xExpectedTicks);
*/

#if ( configUSE_PREEMPTION == 1 )
    if ( pxThreadToSuspend->uxSwitchDeferrals != 0 )
    {
        pxThreadToSuspend->xSwitchDeferred = pdTRUE;
    }
    else
    {
        /* Select Next Task. */
        vTaskSwitchContext();

        pxThreadToResume = prvGetThreadFromTask( xTaskGetCurrentTaskHandle() );

        prvSwitchThread(pxThreadToResume, pxThreadToSuspend);
    }
#endif

    prvRaisePendingInterrupts();

    uxCriticalNesting--;
}
/*-----------------------------------------------------------*/

void vPortThreadDying( void *pxTaskToDelete, volatile BaseType_t *pxPendYield )
{
    Thread_t *pxThread = prvGetThreadFromTask( pxTaskToDelete );

    pxThread->xDying = pdTRUE;
}

void vPortCancelThread( void *pxTaskToDelete )
{
    #if ( CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK )
        /* Call the user defined task pre-deletion hook before canceling the thread */
        extern void vTaskPreDeletionHook( void * pxTCB );
        vTaskPreDeletionHook( pxTaskToDelete );
    #endif /* CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK */

    Thread_t *pxThreadToCancel = prvGetThreadFromTask( pxTaskToDelete );

    /*
     * The thread has already been suspended so it can be safely cancelled.
     */
    pthread_cancel( pxThreadToCancel->pthread );
    pthread_join( pxThreadToCancel->pthread, NULL );
    event_delete( pxThreadToCancel->ev );
}
/*-----------------------------------------------------------*/

static void *prvWaitForStart( void * pvParams )
{
    Thread_t *pxThread = pvParams;

    prvSuspendSelf(pxThread);

    /* Resumed for the first time, unblocks all signals. */
    uxCriticalNesting = 0;
    vPortEnableInterrupts();

    /* Call the task's entry point. */
    pxThread->pxCode( pxThread->pvParams );

    /* A function that implements a task must not exit or attempt to return to
    * its caller as there is nothing to return to. If a task wants to exit it
    * should instead call vTaskDelete( NULL ). Artificially force an assert()
    * to be triggered if configASSERT() is defined, so application writers can
        * catch the error. */
    configASSERT( pdFALSE );

    return NULL;
}
/*-----------------------------------------------------------*/

static void prvSwitchThread( Thread_t *pxThreadToResume,
                             Thread_t *pxThreadToSuspend )
{
    BaseType_t uxSavedCriticalNesting;

    if ( pxThreadToSuspend != pxThreadToResume )
    {
        /*
         * Switch tasks.
         *
         * The critical section nesting is per-task, so save it on the
         * stack of the current (suspending thread), restoring it when
         * we switch back to this task.
         */
        uxSavedCriticalNesting = uxCriticalNesting;

        __atomic_store_n( &pxRunningThread, pxThreadToResume, __ATOMIC_SEQ_CST );
        prvResumeThread( pxThreadToResume );
        if ( pxThreadToSuspend->xDying )
        {
            pthread_exit( NULL );
        }
        prvSuspendSelf( pxThreadToSuspend );

        uxCriticalNesting = uxSavedCriticalNesting;
    }
}
/*-----------------------------------------------------------*/

static void prvSuspendSelf( Thread_t *thread )
{
    /*
     * Suspend this thread by waiting for a pthread_cond_signal event.
     *
     * A suspended thread must not handle signals (interrupts) so
     * all signals must be blocked by calling this from:
     *
     * - Inside a critical section (vPortEnterCritical() /
     *   vPortExitCritical()).
     *
     * - From a signal handler that has all signals masked.
     *
     * - A thread with all signals blocked with pthread_sigmask().
        */
    event_wait(thread->ev);
}

/*-----------------------------------------------------------*/

static void prvResumeThread( Thread_t *xThreadId )
{
    if ( pthread_self() != xThreadId->pthread )
    {
        event_signal(xThreadId->ev);
    }
}
/*-----------------------------------------------------------*/

static void prvSetupSignalsAndSchedulerPolicy( void )
{
    struct sigaction sigresume, sigtick, siginterrupt;
    int iRet;

    hMainThread = pthread_self();

    /* Initialise common signal masks. */
    sigfillset( &xAllSignals );
    /* Don't block SIGINT so this can be used to break into GDB while
     * in a critical section. */
    sigdelset( &xAllSignals, SIGINT );

    /*
     * Block all signals in this thread so all new threads
     * inherits this mask.
     *
     * When a thread is resumed for the first time, all signals
     * will be unblocked.
     */
    (void)pthread_sigmask( SIG_SETMASK, &xAllSignals,
                           &xSchedulerOriginalSignalMask );

    /* SIG_RESUME is only used with sigwait() so doesn't need a
       handler. */
    sigresume.sa_flags = 0;
    sigresume.sa_handler = SIG_IGN;
    sigfillset( &sigresume.sa_mask );

    sigtick.sa_flags = 0;
    sigtick.sa_handler = vPortSystemTickHandler;
    sigfillset( &sigtick.sa_mask );

    iRet = sigaction( SIG_RESUME, &sigresume, NULL );
    if ( iRet )
    {
        prvFatalError( "sigaction", errno );
    }

    iRet = sigaction( SIGALRM, &sigtick, NULL );
    if ( iRet )
    {
        prvFatalError( "sigaction", errno );
    }

    siginterrupt.sa_flags = 0;
    siginterrupt.sa_handler = prvInterruptHandler;
    sigfillset( &siginterrupt.sa_mask );

    iRet = sigaction( SIG_INTERRUPT, &siginterrupt, NULL );
    if ( iRet )
    {
        prvFatalError( "sigaction", errno );
    }
}
/*-----------------------------------------------------------*/

unsigned long ulPortGetRunTime( void )
{
    struct tms xTimes;

    times( &xTimes );

    return ( unsigned long ) xTimes.tms_utime;
}
/*-----------------------------------------------------------*/

void vPortSetStackWatchpoint( void *pxStackStart )
{
    (void) pxStackStart;
}
/*-----------------------------------------------------------*/
