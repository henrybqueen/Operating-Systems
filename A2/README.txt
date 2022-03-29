In this assignment, we were tasked with designing a basic thread scheduler using pthreads. It constists of two kernel level threads, 
one which handles the actual scheduling and another which handles I/O operations. 

----------------------

We use the threaddesc struct to represent tasks, and the IOrequest struct to represent IO requests. 
There are three queues:
    1) The ready queue is populated by tasks (threaddesc objects) that are ready to be run by CEXEC 
    2) The IOqueue, which is populated by IOrequests waiting to be services by IEXEC
    3) The IOoutqueue, which is populated by the result of IO operations done by IEXEC.

Note that IOrequests contain a threaddesc, and when IEXEC is finished servicing an IOrequest, 
it simply places this threaddesc back into the readyqueue. 

If a task makes an IOrequest, by the time it is at the head of the ready queue again,
the response from IECEX will be at the head of the IOout queue, so we can just pop the
head of the IOout queue to obtain the result of the IO operation.

Note that we use pthread_key_t objects to keep track of the context of both CEXEC threads, as well
as the contexts of the jobs currently running on the CEXEC threads.
