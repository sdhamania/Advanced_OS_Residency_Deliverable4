#include "scheduler.hpp"
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
using simulation::ScheduledProcess;
using simulation::runSchedule;
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid input accepted");
}
int main() {
    const std::vector<ScheduledProcess> all{{"A",5,2,0},{"B",3,1,0},{"C",1,1,0}};
    const auto rr=runSchedule(all,true,2,0);
    check(rr.finish==9 && rr.dispatches==6,"RR finish and dispatch count");
    check(rr.processes[0].completion==9 && rr.processes[1].completion==8 && rr.processes[2].completion==5,"RR completion");
    check(rr.processes[0].waiting==4 && rr.processes[1].waiting==5 && rr.processes[2].waiting==4,"RR waiting");
    check(rr.processes[1].response==2 && rr.processes[2].response==4,"RR response");
    check(rr.slices[2].id=="C" && rr.slices[2].end-rr.slices[2].start==1,"short process yields before quantum");
    const auto stagger=runSchedule({{"A",4,2,0},{"B",1,2,2}},true,2,0);
    check(stagger.slices[1].id=="B" && stagger.processes[1].completion==3,"arrival at expiry before requeue");
    const auto inside=runSchedule({{"A",4,2,0},{"B",1,2,1}},true,2,0);
    check(inside.slices[0].end==2 && inside.processes[1].first_start==2,"RR arrival does not preempt current slice");
    const auto priority=runSchedule({{"A",6,3,0},{"B",2,1,2},{"C",1,1,2}},false,1,0);
    check(priority.preemptions==1 && priority.slices[0].end==2,"strict priority arrival preempts immediately");
    check(priority.processes[0].completion==9 && priority.processes[1].completion==4 && priority.processes[2].completion==5,"priority and FCFS tie");
    check(priority.processes[0].waiting==3 && priority.processes[2].response==2,"priority metrics");
    const auto peers=runSchedule({{"A",4,3,0},{"B",1,3,0},{"H",1,1,1}},false,1,0);
    check(peers.slices[2].id=="A" && peers.processes[1].first_start==5,"preempted FCFS peer retains place");
    const auto equal=runSchedule({{"A",4,1,0},{"B",1,1,1}},false,1,0);
    check(equal.preemptions==0 && equal.processes[1].first_start==4,"equal priority arrival does not preempt");
    const auto boundary=runSchedule({{"A",2,3,0},{"B",1,1,2}},false,1,0);
    check(boundary.preemptions==0 && boundary.processes[1].first_start==2,"completion at arrival is not preemption");
    const auto idle=runSchedule({{"A",1,0,3},{"B",1,0,8}},false,1,0);
    check(idle.finish==9 && idle.processes[0].waiting==0 && idle.processes[1].response==0,"CPU idle and metric origin");
    const auto timer=runSchedule({{"A",3,0,0}},false,1,10);
    check(timer.elapsed_ms>=25,"timer must actually wait");
    rejects([]{runSchedule({},true,1,0);});
    rejects([&]{runSchedule(all,true,0,0);});
    rejects([&]{runSchedule(all,false,1,-1);});
    rejects([]{runSchedule({{"A",0,0,0}},true,1,0);});
    rejects([]{runSchedule({{"A",1,0,0},{"A",1,0,1}},true,1,0);});
    rejects([]{runSchedule({{"bad id",1,0,0}},true,1,0);});
    rejects([]{runSchedule({{"A",1,-1,0}},true,1,0);});
    rejects([]{runSchedule({{"A",100000,0,0}},true,1,1);});
    std::cout << "Scheduler unit tests passed (known schedules, arrivals, FCFS, timer, invalid inputs)\n";
}
