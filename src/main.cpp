#include <iostream>
#include <string>
#include <list>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include "configreader.h"
#include "process.h"

// Shared data for all cores
typedef struct SchedulerData {
    std::mutex mutex;
    std::condition_variable condition;
    ScheduleAlgorithm algorithm;
    uint32_t context_switch;
    uint32_t time_slice;
    std::list<Process*> ready_queue;
    bool all_terminated;
} SchedulerData;

void coreRunProcesses(uint8_t core_id, SchedulerData *data);
int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex);
void clearOutput(int num_lines);
uint64_t currentTime();
std::string processStateToString(Process::State state);

int main(int argc, char **argv)
{
    // Ensure user entered a command line parameter for configuration file name
    if (argc < 2)
    {
        std::cerr << "Error: must specify configuration file" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Declare variables used throughout main
    int i;
    SchedulerData *shared_data;
    std::vector<Process*> processes;

    // Read configuration file for scheduling simulation
    SchedulerConfig *config = readConfigFile(argv[1]);

    // Store configuration parameters in shared data object
    uint8_t num_cores = config->cores;
    shared_data = new SchedulerData();
    shared_data->algorithm = config->algorithm;
    shared_data->context_switch = config->context_switch;
    shared_data->time_slice = config->time_slice;
    shared_data->all_terminated = false;

    // Create processes
    uint64_t start = currentTime();
    for (i = 0; i < config->num_processes; i++)
    {
        Process *p = new Process(config->processes[i], start);
        processes.push_back(p);
        // If process should be launched immediately, add to ready queue
        if (p->getState() == Process::State::Ready)
        {
            p->setReadyStartTime(currentTime());
            shared_data->ready_queue.push_back(p);
            
        }
    }

    // Sort the inital ready queue
    {
        std::lock_guard<std::mutex> lock(shared_data->mutex);
        if(!shared_data->ready_queue.empty())
        {
            SjfComparator sjf;
            if (shared_data->algorithm == ScheduleAlgorithm::SJF) {
                shared_data->ready_queue.sort(sjf);
            }
            PpComparator pp;
            if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                shared_data->ready_queue.sort(pp);
            }
        }
    }
    

    // Free configuration data from memory
    deleteConfig(config);

    // Launch 1 scheduling thread per cpu core
    std::thread *schedule_threads = new std::thread[num_cores];
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i] = std::thread(coreRunProcesses, i, shared_data);
    }
    
    // Main thread work goes here
    int num_lines = 0;
    while (!(shared_data->all_terminated))
    {
        // Clear output from previous iteration
        clearOutput(num_lines);
        
        // Do the following:
        //   - Get current time
        uint64_t current_time = currentTime();
        
        //   - *Check if any processes need to move from NotStarted to Ready (based on elapsed time), and if so put that process in the ready queue
        // need to check if start time >= process launch time, if it is, add to ready queue
        // loop through all the processes
        
        for (i = 0; i < processes.size(); i++)
        {   // check the state of the process
            

            if(processes[i]->getState() == Process::State::NotStarted)
            {   // compare the start time of the process to the elapsed time
                
                if(processes[i]->getStartTime() >= (current_time - start))
                {   // lock the shared data, update state, push the process into the ready queue
                
                    std::lock_guard<std::mutex> lock(shared_data->mutex);
                    processes[i]->setState(Process::State::Ready, current_time);
                    processes[i]->setReadyStartTime(current_time);
                    shared_data->ready_queue.push_back(processes[i]);
                }//if
            }//if
        }//for
    
        //   - *Check if any processes have finished their I/O burst, and if so put that process back in the ready queue
        for (i = 0; i < processes.size(); i++)
        {   
           if(processes[i]->getState() == Process::State::IO && ((current_time - processes[i]->getBurstStartTime()) > processes[i]->getCurrentBurstTime()))
            {// lock the shared data, update state, push the process into the ready queue
                std::lock_guard<std::mutex> lock(shared_data->mutex);
                processes[i]->setState(Process::State::Ready, current_time);
                processes[i]->nextBurst();
                processes[i]->setReadyStartTime(current_time);
                shared_data->ready_queue.push_back(processes[i]);
            }//if
        }//for
    
        //   - *Check if any running process need to be interrupted (RR time slice expires or newly ready process has higher priority)
        // RR check for time splice interrupts
        if (shared_data->algorithm == ScheduleAlgorithm::RR) {
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            for(int i = 0; i < processes.size(); i++)
            {
                if( processes[i]->getState() == Process::State::Running && current_time - processes[i]->getBurstStartTime() > shared_data->time_slice)
                {
                    processes[i]->interrupt();
                }
            }
        }
   
        // PP
        if (shared_data->algorithm == ScheduleAlgorithm::PP) {
            // Checking for running processes with lower priority
            std::lock_guard<std::mutex> lock(shared_data->mutex);

            // Vector to hold the running processes to make it easier
            std::vector<Process *> running_processes;
            for(int i = 0; i < processes.size(); i++)
            {
                if( processes[i]->getState() == Process::State::Running )
                {
                    running_processes.push_back(processes[i]);
                }   
            }
            // Loop through the elements in the ready queue and see if they have a lower priority than anything running
            std::list<Process *>::iterator it;
            for (it = shared_data->ready_queue.begin(); it!= shared_data->ready_queue.end(); it++) {
                Process* current_process = *it;
                bool interrupt = false;             //if an interrupt is needed
                int index = -1;                     //index of the process to be interrupted
                // Check each running process. Find the process with the highest priority and last to enter for interruption if need be.
                for (int i = 0; i < running_processes.size(); i++)
                {
                    // Priority comparison with ready queue process
                    if ( current_process->getPriority() < running_processes[i]->getPriority() ) {
                        if( interrupt )
                        {   
                            // If two processes have a lower priority, choose the one with the lowest priority
                            if(running_processes[i]->getPriority() < running_processes[index]->getPriority() )
                            {
                                index = i;
                            }
                            // If the two running processes have the same priority, choose the most recently running process
                            else if (running_processes[i]->getPriority() == running_processes[index]->getPriority())
                            {
                                if(running_processes[i]->getBurstStartTime() > running_processes[index]->getBurstStartTime())
                                {
                                    index = i;
                                }
                            }
                        }
                        else
                        {
                            interrupt = true;
                            index = i;
                        }
                    }
                } 
                if(interrupt)
                {
                    running_processes[index]->interrupt();
                    running_processes.erase(running_processes.begin() + index);
                }
            }
        }

        //   - *Sort the ready queue (if needed - based on scheduling algorithm)
        {
        std::lock_guard<std::mutex> lock(shared_data->mutex);
        if(!shared_data->ready_queue.empty())
        {
            SjfComparator sjf;
            if (shared_data->algorithm == ScheduleAlgorithm::SJF) {
                shared_data->ready_queue.sort(sjf);
            }
            PpComparator pp;
            if (shared_data->algorithm == ScheduleAlgorithm::PP) {
                shared_data->ready_queue.sort(pp);
            }
        }
        }

        bool temp = true;
        //   - Determine if all processes are in the terminated state
        for(int i = 0;i < processes.size(); i++)
        {
            if(processes[i]->getState() != Process::State::Terminated)
            {
                temp = false;
            }
        }
        shared_data->all_terminated = temp;
        //   - * = accesses shared data (ready queue), so be sure to use proper synchronization

        // output process status table
        num_lines = printProcessOutput(processes, shared_data->mutex);
    
        // sleep 50 ms
        usleep(50000);
    }


    // wait for threads to finish
    for (i = 0; i < num_cores; i++)
    {
        schedule_threads[i].join();
    }

    // print final statistics
    //  - CPU utilization
    double total_wait = 1.0;
    for (i = 0; i < processes.size(); i++) 
    {
        double wait_ratio = processes[i]->getWaitTime() / processes[i]->getTurnaroundTime();
        total_wait = total_wait * wait_ratio;
    }
    double cpu_utl = (1.0 - total_wait) * 100;

    //  - Throughput
    
    
    // sort queue to iterate over easier for 1st and 2nd halves
    for (int i = 0; i < processes.size(); i ++) 
    {
        for (int j = 0; j < processes.size(); j++) 
        {
            if (processes[j]->getTurnaroundTime() > processes[i]->getTurnaroundTime())
            {
                Process *temp = processes[i];
                processes[i] = processes[j];
                processes[j] = temp;
            }
        }
    }

    //     - Average for first 50% of processes finished
    double first_half_total = 0;
    double first_half_avg = 0;
    double first_half_thru;
    for (int i = 0; i < processes.size() / 2; i++) 
    {
        first_half_total = first_half_total + processes[i]->getTurnaroundTime();
        first_half_avg =  first_half_total / (processes.size() / 2);
        first_half_thru = (processes.size() / 2) / first_half_avg;
    }
    
    //     - Average for second 50% of processes finished
    double second_half_total = 0;
    double second_half_avg = 0;
    double second_half_thru;
    for (int i = processes.size() / 2; i < processes.size(); i++) 
    {
        second_half_total = second_half_total + processes[i]->getTurnaroundTime();
        if ((processes.size() % 2) != 0)
        {
            second_half_avg = second_half_total / ((processes.size() / 2) + 1);
            second_half_thru = ((processes.size() / 2) + 1) / second_half_avg;
        } else 
        {
            second_half_avg = second_half_total / (processes.size() / 2);
            second_half_thru = (processes.size() / 2) / second_half_avg;
        }

    }
    
    //     - Overall average
    double max_turn = processes[0]->getTurnaroundTime();
    for (int i = 0; i < processes.size(); i++) 
    {
        if (processes[i]->getTurnaroundTime() > max_turn)
        {
            max_turn = processes[i]->getTurnaroundTime();
        }
    }
    double overall_avg = processes.size() / max_turn;

    //  - Average turnaround time
    double turn_total = 0.0;
    double turn_avg = 0.0;
    for (i = 0; i < processes.size(); i++) 
    {
        turn_total = processes[i]->getTurnaroundTime() + turn_total;
    }
    turn_avg = turn_total / processes.size();

    //  - Average waiting time
    double wait_total = 0.0;
    double wait_avg = 0.0;
    for (i = 0; i < processes.size(); i++) 
    {
        wait_total = processes[i]->getWaitTime() + wait_total;
    }
    wait_avg = wait_total / processes.size();


    printf("\n");
    printf("First Half Throughput Average is %.2f processes per second\n", first_half_thru);
    printf("Second Half Throughput Average is %.2f processes per second\n", second_half_thru);
    printf("Overall Throughput Average is %.2f processes per second\n", overall_avg);
    printf("CPU Utilization is %.2f%%\n", cpu_utl); 
    printf("Turnaround average is %.2f seconds\n", turn_avg);
    printf("Wait average is %.2f seconds\n", wait_avg);
    printf("\n");
    

    
    


    // Clean up before quitting program
    processes.clear();

    return 0;
}

void coreRunProcesses(uint8_t core_id, SchedulerData *shared_data)
{
    // Work to be done by each core independent of the other cores
    // Repeat until all processes in terminated state:
    //   - *Get process at front of ready queue
    //   - Simulate the processes running until one of the following:
    //     - CPU burst time has elapsed
    //     - Interrupted (RR time slice has elapsed or process preempted by higher priority process)
    //  - Place the process back in the appropriate queue
    //     - I/O queue if CPU burst finished (and process not finished) -- no actual queue, simply set state to IO
    //     - Terminated if CPU burst finished and no more bursts remain -- no actual queue, simply set state to Terminated
    //     - *Ready queue if interrupted (be sure to modify the CPU burst time to now reflect the remaining time)
    //  - Wait context switching time
    //  - * = accesses shared data (ready queue), so be sure to use proper synchronization
    while(!shared_data->all_terminated)
    {
        Process* core_process;
        // Get the front of the ready queue
        {
            std::lock_guard<std::mutex> lock(shared_data->mutex);

            core_process = shared_data->ready_queue.front();
            if(core_process != NULL)
            {
                shared_data->ready_queue.pop_front();
                
            }
        }
        if(core_process != NULL){
        core_process->setCpuCore(core_id);
        core_process->updateProcess(currentTime());
        core_process->setState(Process::State::Running, currentTime());

        // Simulate running until burst time elapsed or interrupt
        core_process->setBurstStartTime(currentTime());
        while( ((currentTime() - core_process->getBurstStartTime()) < core_process->getCurrentBurstTime()) && !(core_process->isInterrupted()) ) {}
        uint64_t end_time = currentTime();

        // Update the processes cpu time, burst time, and remain time
        // This also handles the interrupted burst time
        core_process->updateProcess(end_time);

        // Place the process back in the appropriate queue
        if(core_process->isInterrupted())
        {   //into ready queue if interrupted
            std::lock_guard<std::mutex> lock(shared_data->mutex);
            core_process->interruptHandled();
            core_process->setReadyStartTime(currentTime());
            core_process->setState(Process::State::Ready, currentTime());
            shared_data->ready_queue.push_back(core_process);
        }
        else if(core_process->getRemainingTime() > 0)
        {   //more time remaining - IO
            core_process->setState(Process::State::IO, currentTime());
            core_process->nextBurst();
            core_process->setBurstStartTime(currentTime());
        }
        else if(core_process->getRemainingTime() == 0)
        {   //no more process remaining - terminate
            core_process->setState(Process::State::Terminated, currentTime());
            core_process->updateProcess(end_time);
        }
        
        //context switch time
        usleep(shared_data->context_switch);
        core_process->setCpuCore(-1);
        }

    }//while
}

int printProcessOutput(std::vector<Process*>& processes, std::mutex& mutex)
{
    int i;
    int num_lines = 2;
    std::lock_guard<std::mutex> lock(mutex);
    printf("|   PID | Priority |      State | Core | Turn Time | Wait Time | CPU Time | Remain Time |\n");
    printf("+-------+----------+------------+------+-----------+-----------+----------+-------------+\n");
    for (i = 0; i < processes.size(); i++)
    {
        if (processes[i]->getState() != Process::State::NotStarted)
        {
            uint16_t pid = processes[i]->getPid();
            uint8_t priority = processes[i]->getPriority();
            std::string process_state = processStateToString(processes[i]->getState());
            int8_t core = processes[i]->getCpuCore();
            std::string cpu_core = (core >= 0) ? std::to_string(core) : "--";
            double turn_time = processes[i]->getTurnaroundTime();
            double wait_time = processes[i]->getWaitTime();
            double cpu_time = processes[i]->getCpuTime();
            double remain_time = processes[i]->getRemainingTime();
            printf("| %5u | %8u | %10s | %4s | %9.1lf | %9.1lf | %8.1lf | %11.1lf |\n", 
                   pid, priority, process_state.c_str(), cpu_core.c_str(), turn_time, 
                   wait_time, cpu_time, remain_time);
            num_lines++;
        }
    }
    return num_lines;
}

void clearOutput(int num_lines)
{
    int i;
    for (i = 0; i < num_lines; i++)
    {
        fputs("\033[A\033[2K", stdout);
    }
    rewind(stdout);
    fflush(stdout);
}

uint64_t currentTime()
{
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return ms;
}

std::string processStateToString(Process::State state)
{
    std::string str;
    switch (state)
    {
        case Process::State::NotStarted:
            str = "not started";
            break;
        case Process::State::Ready:
            str = "ready";
            break;
        case Process::State::Running:
            str = "running";
            break;
        case Process::State::IO:
            str = "i/o";
            break;
        case Process::State::Terminated:
            str = "terminated";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
