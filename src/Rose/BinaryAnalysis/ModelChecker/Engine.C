#include <featureTests.h>
#ifdef ROSE_ENABLE_BINARY_ANALYSIS
#include <sage3basic.h>
#include <Rose/BinaryAnalysis/ModelChecker/Engine.h>

#include <Rose/BinaryAnalysis/ModelChecker/ExecutionUnit.h>
#include <Rose/BinaryAnalysis/ModelChecker/P2Model.h>
#include <Rose/BinaryAnalysis/ModelChecker/Path.h>
#include <Rose/BinaryAnalysis/ModelChecker/PathNode.h>
#include <Rose/BinaryAnalysis/ModelChecker/PathPredicate.h>
#include <Rose/BinaryAnalysis/ModelChecker/PathPrioritizer.h>
#include <Rose/BinaryAnalysis/ModelChecker/SemanticCallbacks.h>
#include <Rose/BinaryAnalysis/ModelChecker/Settings.h>
#include <Rose/BinaryAnalysis/ModelChecker/SourceLister.h>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/scope_exit.hpp>
#include <Rose/BinaryAnalysis/InstructionSemantics2/TraceSemantics.h>
#include <Rose/BinaryAnalysis/Partitioner2/BasicBlock.h>
#include <Rose/BinaryAnalysis/Partitioner2/Function.h>

#ifdef __linux
#include <sys/syscall.h>                                // SYS_* constants
#include <sys/types.h>                                  // pid_t
#include <unistd.h>                                     // syscall
#endif


using namespace Sawyer::Message::Common;
namespace IS = Rose::BinaryAnalysis::InstructionSemantics2;
namespace BS = Rose::BinaryAnalysis::InstructionSemantics2::BaseSemantics;
namespace P2 = Rose::BinaryAnalysis::Partitioner2;

namespace Rose {
namespace BinaryAnalysis {
namespace ModelChecker {

Sawyer::Message::Facility mlog;

void
initDiagnostics() {
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        Diagnostics::initAndRegister(&mlog, "Rose::BinaryAnalysis::ModelChecker");
        mlog.comment("model checking");
    }
}

static int
rose_gettid() {
#ifdef __linux
    // Newer versions of glibc define gettid, but we might as well just use syscall since we still (2021) need to support
    // RedHat 6 and 7 where gettid is not available.
    return syscall(SYS_gettid);
#else
    return 0;
#endif
}

Engine::InProgress::InProgress()
    : tid(rose_gettid()) {}

Engine::InProgress::InProgress(const Path::Ptr &path)
    : path(path), threadId(boost::this_thread::get_id()), tid(rose_gettid()) {}

Engine::InProgress::~InProgress() {}

Engine::Engine(const Settings::Ptr &settings)
    : frontier_(LongestPathFirst::instance()), interesting_(ShortestPathFirst::instance()),
      frontierPredicate_(WorkPredicate::instance()), interestingPredicate_(HasFinalTags::instance()),
      settings_(settings) {}

Engine::Ptr
Engine::instance() {
    SAWYER_MESG_FIRST(mlog[WHERE], mlog[TRACE], mlog[DEBUG]) <<"entering configuration phase\n";
    return Ptr(new Engine(Settings::instance()));
}

Engine::Ptr
Engine::instance(const Settings::Ptr &settings) {
    ASSERT_not_null(settings);
    if (!settings->sourceLister)
        settings->sourceLister = SourceLister::instance();
    SAWYER_MESG_FIRST(mlog[WHERE], mlog[TRACE], mlog[DEBUG]) <<"entering configuration phase\n";
    return Ptr(new Engine(settings));
}

Engine::~Engine() {
    for (std::thread &t: workers_)
        t.join();
}

Settings::Ptr
Engine::settings() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    ASSERT_not_null(settings_);
    return settings_;
}

void
Engine::settings(const Settings::Ptr &s) {
    ASSERT_not_null(s);
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    settings_ = s;
    if (!settings_->sourceLister)
        settings_->sourceLister = SourceLister::instance();
}

SemanticCallbacks::Ptr
Engine::semantics() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return semantics_;
}

void
Engine::semantics(const SemanticCallbacks::Ptr &sem) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    semantics_ = sem;
}

PathPrioritizer::Ptr
Engine::explorationPrioritizer() const {
    return frontier_.prioritizer();
}

void
Engine::explorationPrioritizer(const PathPrioritizer::Ptr &prio) {
    frontier_.prioritizer(prio);
}

PathPredicate::Ptr
Engine::explorationPredicate() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    ASSERT_not_null(frontierPredicate_);
    return frontierPredicate_;
}

void
Engine::explorationPredicate(const PathPredicate::Ptr &pred) {
    ASSERT_not_null(pred);
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    frontierPredicate_ = pred;
}

PathPredicate::Ptr
Engine::interestingPredicate() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    ASSERT_not_null(interestingPredicate_);
    return interestingPredicate_;
}

void
Engine::interestingPredicate(const PathPredicate::Ptr &pred) {
    ASSERT_not_null(pred);
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    interestingPredicate_ = pred;
}

void
Engine::reset() {
    // Reap all managed workers
    for (std::thread &t: workers_)
        t.join();
    workers_.clear();

    // Make sure no user threads are working
    ASSERT_require(0 == workCapacity_);
    ASSERT_require(0 == nWorking_);
    ASSERT_forbid(stopping_);

    // Reset statistics
    nStepsExplored_ = nPathsExplored_ = 0;
    fanout_.clear();
    semantics_->reset();
    frontierPredicate_->reset();
    interestingPredicate_->reset();
    elapsedTime_.clear();
    timeSinceStats_.clear();
    nPathsStats_ = 0;

    // Reset priority queues
    frontier_.reset();
    interesting_.reset();
    inProgress_.clear();
}

void
Engine::insertStartingPoint(const ExecutionUnit::Ptr &unit) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    ASSERT_not_null(unit);
    ASSERT_not_null(frontierPredicate_);
    ASSERT_not_null(semantics_);
    auto path = Path::instance(unit);
    frontier_.insert(path);                             // intentionally not checking the insertion predicate
    SAWYER_MESG(mlog[DEBUG]) <<"starting at " <<unit->printableName() <<"\n";
    newWork_.notify_all();
}

void
Engine::startWorkers(size_t n) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    if (0 == n) {
        size_t hardware = std::thread::hardware_concurrency();
        if (0 == hardware) {
            // We don't know how much hardware parallelism there is, and the user apparently doesn't really care,
            // so choose something that will probably work on nearly every reasonable machine, including laptops.
            hardware = 4;
        }
        if (hardware > workers_.size()) {
            n = hardware - workers_.size();
        } else {
            return;
        }
    }

    if (n > 0) {
        SAWYER_MESG_FIRST(mlog[WHERE], mlog[TRACE], mlog[DEBUG]) <<"starting " <<StringUtility::plural(n, "workers") <<"\n";
        for (size_t i = 0; i < n; ++i) {
            ++workCapacity_;
            workers_.push_back(std::thread([this](){worker();}));
        }
    }
}

void
Engine::run() {
    while (workRemains()) {
        if (0 == workCapacity()) {
            step(); // we're the only thread working (at the moment we checked)
        } else {
            // Wait for other threads to stop working or for new work to be created
            SAWYER_THREAD_TRAITS::UniqueLock lock(mutex_);
            newWork_.wait(lock);
        }
    }
}

void
Engine::stop() {
    SAWYER_THREAD_TRAITS::UniqueLock lock(mutex_);
    stopping_ = true;
    newWork_.notify_all();
    while (workCapacity_ > 0)
        newWork_.wait(lock);
    for (std::thread &t: workers_)
        t.join();
    workers_.clear();
    inProgress_.clear();
    stopping_ = false;
}

// called only by user threads
bool
Engine::step() {
    // Show when this user thread starts and ends work
    WorkerState state = WorkerState::STARTING;
    changeState(state, WorkerState::STARTING);
    BOOST_SCOPE_EXIT(this_, &state) {
        this_->changeState(state, WorkerState::FINISHED);
    } BOOST_SCOPE_EXIT_END;

    // Create a thread-local RISC operators that will be used to update semantic states
    ASSERT_not_null(semantics_);
    BS::RiscOperators::Ptr ops = semantics_->createRiscOperators();
    ASSERT_not_null(ops);
    ASSERT_require2(ops->initialState() == nullptr, "initial states are not supported during model checking");
    ASSERT_require2(ops->currentState() == nullptr, "please remove the current state for added safety");
    SmtSolver::Ptr solver = semantics_->createSolver();
    ASSERT_not_null(ops);

    // Do one step of work if work is immediately available
    bool retval = false;
    if (Path::Ptr path = takeNextWorkItemNow(state)) {  // returns immediately, not waiting for new work
        BOOST_SCOPE_EXIT(this_, &ops) {
            this_->finishPath(ops);
        } BOOST_SCOPE_EXIT_END;
        doOneStep(path, ops, solver);
        return true;
    }

    return retval;
}

// called only by managed worker threads.
void
Engine::worker() {
    // Show when this managed worker starts and ends work
    WorkerState state = WorkerState::STARTING;          // the caller has already changed our state for us.
    BOOST_SCOPE_EXIT(this_, &state) {
        this_->changeState(state, WorkerState::FINISHED);
    } BOOST_SCOPE_EXIT_END;

    // Create a thread-local RISC operators that will be used to update semantic states
    ASSERT_not_null(semantics_);
    BS::RiscOperators::Ptr ops = semantics_->createRiscOperators();
    ASSERT_not_null(ops);
    ASSERT_require2(ops->initialState() == nullptr, "initial states are not supported during model checking");
    ASSERT_require2(ops->currentState() == nullptr, "please remove the current state for added safety");
    SmtSolver::Ptr solver = semantics_->createSolver();
    ASSERT_not_null(solver);

    changeState(state, WorkerState::WAITING);
    while (Path::Ptr path = takeNextWorkItem(state)) {
        BOOST_SCOPE_EXIT(this_, &ops) {
            this_->finishPath(ops);
        } BOOST_SCOPE_EXIT_END;
        doOneStep(path, ops, solver);
        changeState(state, WorkerState::WAITING);
    }
}

bool
Engine::workRemains() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return !frontier_.isEmpty() || nWorking_ > 0;
}

size_t
Engine::workCapacity() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return workCapacity_;
}

size_t
Engine::nWorking() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return nWorking_;
}

size_t
Engine::nPathsPending() const {
    // No lock necessary (it happens inside frontier_ instead).
    return frontier_.size();
}

const PathQueue&
Engine::pendingPaths() const {
    // No lock necessary since frontier_'s address doesn't ever change.
    return frontier_;
}

std::vector<Engine::InProgress>
Engine::inProgress() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    std::vector<Engine::InProgress> retval;
    retval.reserve(inProgress_.size());
    for (const InProgress &record: inProgress_.values())
        retval.push_back(record);
    return retval;
}

const PathQueue&
Engine::interesting() const {
    // No lock necessary since interesting_'s address is constant.
    return interesting_;
}

PathQueue&
Engine::interesting() {
    // No lock necessary since interesting_'s address is constant.
    return interesting_;
}

bool
Engine::insertInteresting(const Path::Ptr &path) {
    ASSERT_not_null(path);
    ASSERT_not_null(interestingPredicate_);
    auto p = interestingPredicate_->test(settings_, path);
    if (p.first) {
        SAWYER_MESG(mlog[DEBUG]) <<"  interesting (" <<p.second <<") " <<path->printableName() <<"\n";
        interesting_.insert(path);
        newInteresting_.notify_one();
        return true;
    } else {
        SAWYER_MESG(mlog[DEBUG]) <<"  uninteresting (" <<p.second <<") " <<path->printableName() <<"\n";
        return false;
    }
}

bool
Engine::insertWork(const Path::Ptr &path) {
    ASSERT_not_null(path);
    ASSERT_not_null(frontierPredicate_);
    auto p = frontierPredicate_->test(settings_, path);
    if (p.first) {
        SAWYER_MESG(mlog[DEBUG]) <<"    inserted work (" <<p.second <<") " <<path->printableName() <<"\n";
        frontier_.insert(path);
        newWork_.notify_one();
        return true;
    } else {
        SAWYER_MESG(mlog[DEBUG]) <<"    rejected work (" <<p.second <<") " <<path->printableName() <<"\n";
        return false;
    }
}

Path::Ptr
Engine::takeNextWorkItemNow(WorkerState &state) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    if (stopping_)
        return Path::Ptr();
    Path::Ptr retval = frontier_.takeNext();
    if (retval) {
        changeStateNS(state, WorkerState::WORKING);
        inProgress_.insert(boost::this_thread::get_id(), InProgress(retval));
    }
    return retval;
}

Path::Ptr
Engine::takeNextWorkItem(WorkerState &state) {
    SAWYER_THREAD_TRAITS::UniqueLock lock(mutex_);
    while (true) {
        if (stopping_)
            return Path::Ptr();
        if (Path::Ptr retval = frontier_.takeNext()) {
            changeStateNS(state, WorkerState::WORKING);
            inProgress_.insert(boost::this_thread::get_id(), InProgress(retval));
            return retval;
        }
        if (0 == nWorking_)
            return Path::Ptr();
        newWork_.wait(lock);
    }
}

void
Engine::finishPath(const BS::RiscOperators::Ptr &ops) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    inProgress_.erase(boost::this_thread::get_id());

    IS::SymbolicSemantics::RiscOperators::Ptr symbolicOps;
    if (auto traceSemantics = boost::dynamic_pointer_cast<IS::TraceSemantics::RiscOperators>(ops)) {
        symbolicOps = IS::SymbolicSemantics::RiscOperators::promote(traceSemantics->subdomain());
    } else {
        symbolicOps = IS::SymbolicSemantics::RiscOperators::promote(ops);
    }

    nExpressionsTrimmed_ += symbolicOps->nTrimmed();
    symbolicOps->nTrimmed(0);
}

Path::Ptr
Engine::takeNextInteresting() {
    SAWYER_THREAD_TRAITS::UniqueLock lock(mutex_);
    while (true) {
        if (Path::Ptr retval = interesting_.takeNext())
            return retval;
        if (0 == workCapacity_)
            return Path::Ptr();
        newInteresting_.wait(lock);
    }
}

void
Engine::changeState(WorkerState &cur, WorkerState next) {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    changeStateNS(cur, next);
}

void
Engine::startWorkingNS() {
    ++workCapacity_;
    if (!elapsedTime_.isRunning())
        elapsedTime_.restart();
}

void
Engine::finishWorkingNS() {
    --workCapacity_;
    newWork_.notify_all();
    newInteresting_.notify_all();
}

void
Engine::changeStateNS(WorkerState &cur, WorkerState next) {
    switch (cur) {
        case WorkerState::STARTING:
            switch (next) {
                case WorkerState::STARTING:
                    startWorkingNS();
                    break;
                case WorkerState::WAITING:
                    ASSERT_require(workCapacity_ > 0);
                    break;
                case WorkerState::WORKING:
                    ASSERT_require(workCapacity_ > 0);
                    ++nWorking_;
                    ASSERT_require(nWorking_ <= workCapacity_);
                    break;
                case WorkerState::FINISHED:
                    ASSERT_require(workCapacity_ > 0);
                    finishWorkingNS();
                    break;
            }
            break;

        case WorkerState::WAITING:
            ASSERT_require(workCapacity_ > 0);
            switch (next) {
                case WorkerState::STARTING:
                    ASSERT_not_reachable("invalid worker transition: waiting -> starting");
                case WorkerState::WAITING:
                    break;
                case WorkerState::WORKING:
                    ASSERT_require(nWorking_ < workCapacity_);
                    ++nWorking_;
                    break;
                case WorkerState::FINISHED:
                    finishWorkingNS();
                    break;
            }
            break;

        case WorkerState::WORKING:
            ASSERT_require(workCapacity_ > 0);
            ASSERT_require(nWorking_ > 0);
            switch (next) {
                case WorkerState::STARTING:
                    ASSERT_not_reachable("invalid worker transition: working -> starting");
                case WorkerState::WAITING:
                    --nWorking_;
                    break;
                case WorkerState::WORKING:
                    ASSERT_not_reachable("invalid worker transition: working -> working");
                case WorkerState::FINISHED:
                    --nWorking_;
                    finishWorkingNS();
                    break;
            }
            break;

        case WorkerState::FINISHED:
            ASSERT_not_reachable("invalid worker transition from finished state");
    }
    cur = next;
}

// An entry in a variable index that says where a variable is constrained along a path.
struct VarIndexEntry {
    size_t pathNodeIndex;                               // 0 is beginning of the path
    PathNode::Ptr pathNode;
    size_t assertionIndex;                              // 0 is first assertion of node. "true" is not counted.
};

// An index describing each symbolic variable that appears in a constraint along the path.
using AssertionIndex = Sawyer::Container::Map<uint64_t /*ID*/, std::vector<VarIndexEntry>>;

// Create an index of all the variables in a particular expression. Used with SymbolicExpr::Node::depthFirstTraversal.
class ExprIndexer: public SymbolicExpr::Visitor {
public:
    AssertionIndex index;

    // Where is the assertion that we're currently scanning?
    size_t pathNodeIndex;
    PathNode::Ptr pathNode;
    size_t assertionIndex = 0;

    // Call this before scanning a new assertion expression.
    void current(size_t pathNodeIndex, const PathNode::Ptr &pathNode, size_t assertionIndex) {
        this->pathNodeIndex = pathNodeIndex;
        this->pathNode = pathNode;
        this->assertionIndex = assertionIndex;
    }

    virtual SymbolicExpr::VisitAction preVisit(const SymbolicExpr::Node *node) {
        if (auto id = node->variableId()) {
            auto &entryList = index.insertMaybeDefault(*id);
            if (entryList.empty() || entryList.back().assertionIndex != assertionIndex)
                entryList.push_back(VarIndexEntry{.pathNodeIndex = pathNodeIndex,
                                                  .pathNode = pathNode,
                                                  .assertionIndex = assertionIndex});
        }
        return SymbolicExpr::CONTINUE;
    }

    virtual SymbolicExpr::VisitAction postVisit(const SymbolicExpr::Node*) {
        return SymbolicExpr::CONTINUE;
    }
};

void
Engine::displaySmtAssertions(const Path::Ptr &path) {
    if (mlog[DEBUG]) {
        Sawyer::Message::Stream debug(mlog[DEBUG]);
        SymbolicExpr::Ptr t = SymbolicExpr::makeBooleanConstant(true);

        // Create an index of all the variables for all the assertions for all the nodes of the path.
        ExprIndexer indexer;
        auto nodes = path->nodes();
        for (size_t i = 0, assertionIdx = 0; i < nodes.size(); ++i) {
            auto assertions = nodes[i]->assertions();
            for (const SymbolicExpr::Ptr &assertion: assertions) {
                if (!assertion->isEquivalentTo(t)) {
                    indexer.current(i, nodes[i], assertionIdx++);
                    assertion->depthFirstTraversal(indexer);
                }
            }
        }

        // Print the index
        if (!indexer.index.isEmpty()) {
            debug <<"  variables appearing in assertions:\n";
            for (const auto &node: indexer.index.nodes()) {
                debug <<"    v" <<node.key() <<":\n";
                for (const VarIndexEntry &entry: node.value()) {
                    debug <<"      mentioned at assertion " <<entry.assertionIndex
                          <<" of node " <<entry.pathNodeIndex <<": " <<entry.pathNode->printableName() <<"\n";
                }
            }
        }

        // Print the assertions
        debug <<"  assertions (path constraints):\n";
        for (size_t i = 0, assertionIdx = 0; i < nodes.size(); ++i) {
            auto assertions = nodes[i]->assertions();
            for (const SymbolicExpr::Ptr &assertion: assertions) {
                if (!assertion->isEquivalentTo(t)) {
                    debug <<"    assertion " <<assertionIdx
                          <<" at node " <<i <<" " <<nodes[i]->printableName() <<"\n";
                    debug <<"      " <<*assertion <<"\n";
                }
            }
        }
    }
}

void
Engine::execute(const Path::Ptr &path, const BS::RiscOperators::Ptr &ops, const SmtSolver::Ptr &solver) {
    ASSERT_not_null(path);
    ASSERT_forbid(path->isEmpty());

    if (mlog[DEBUG]) {
        mlog[DEBUG] <<"  current path:\n";
        auto nodes = path->nodes();
        for (size_t i = 0; i < nodes.size(); ++i)
            SAWYER_MESG(mlog[DEBUG]) <<boost::format("    node %-3d: %s\n") % i % nodes[i]->printableName();
    }

    if (mlog[DEBUG] && settings()->showAssertions)
        displaySmtAssertions(path);

    {
        size_t nsteps = path->lastNode()->nSteps();
        SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
        ++nPathsExplored_;
        nStepsExplored_ += nsteps;
    }

    path->lastNode()->execute(settings_, semantics_, ops, solver);
    insertInteresting(path);
}

size_t
Engine::nPathsExplored() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return nPathsExplored_;
}

size_t
Engine::nStepsExplored() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return nStepsExplored_;
}

size_t
Engine::nExpressionsTrimmed() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return nExpressionsTrimmed_;
}

void
Engine::extend(const Path::Ptr &path, const BS::RiscOperators::Ptr &ops, const SmtSolver::Ptr &solver) {
    ASSERT_not_null(path);
    ASSERT_not_null(ops);
    ASSERT_not_null(semantics_);
    SAWYER_MESG(mlog[DEBUG]) <<"  extending path\n";
    size_t nChildren = 0;

    if (path->executionFailed()) {
        SAWYER_MESG(mlog[DEBUG]) <<"    execution failure; no extension possible\n";
    } else {
        // Get the list execution units that would be executed after this path
        PathNode::Ptr current = path->lastNode();
        std::vector<SemanticCallbacks::NextUnit> nextUnits;
        BS::State::Ptr parentOutgoingState;
        {
            auto borrowed = current->borrowOutgoingState();
            parentOutgoingState = borrowed.state;
            ASSERT_require(ops->initialState() == nullptr); // initial states are not supported
            ASSERT_require(ops->currentState() == nullptr); // extra safety check
            ops->currentState(borrowed.state);
            solver->reset();
            solver->insert(path->assertions());
            BOOST_SCOPE_EXIT(&ops) {
                ops->currentState(nullptr);
            } BOOST_SCOPE_EXIT_END;
            nextUnits = semantics_->nextUnits(path, ops, solver);
        }

        // For each execution unit, extend the current path by that one unit and add it as new work.
        ASSERT_not_null(parentOutgoingState);
        for (const SemanticCallbacks::NextUnit &next: nextUnits) {
            auto newPath = Path::instance(path, next.unit, next.assertion, next.evidence, parentOutgoingState);
            if (insertWork(newPath))
                ++nChildren;
        }

        // Path no longer needs its outgoing state since we copied it to the incoming state of each of the new
        // paths we created.
        path->lastNode()->releaseOutgoingState();
    }
    updateFanout(nChildren, path->nSteps(), path->lastNode()->nSteps());
}

void
Engine::updateFanout(size_t nChildren, size_t totalSteps, size_t lastSteps) {
    ASSERT_require(totalSteps >= lastSteps);
    if (0 == lastSteps)
        return;

    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);

    // Extend the fanout_ vector large enough to hold the results
    if (totalSteps-1 >= fanout_.size())
        fanout_.resize(totalSteps, std::make_pair(0.0, size_t(0)));

    // Increment the fanout for the internal steps. They all have a fanout of 1
    for (size_t i = totalSteps - lastSteps; i < totalSteps-1; ++i) {
        fanout_[i].first += 1.0;
        ++fanout_[i].second;
    }

    // Increment the fanout for the last step.
    fanout_[totalSteps-1].first += nChildren;
    ++fanout_[totalSteps-1].second;

    // Increment the number of times we see a root node
    if (lastSteps == totalSteps)
        ++nFanoutRoots_;
}

double
Engine::estimatedForestSize(size_t k) const {
    Sawyer::Message::Stream debug(mlog[DEBUG]);
#if 1 // this produces a lot of output, so we normally disable it
    debug.enable(false);
#endif

    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    double estRow = std::max((double)nFanoutRoots_, 1.0), estTotal = 0.0, factorTotal = 0.0;
    size_t actualTotal = 0;
    SAWYER_MESG(debug) <<(boost::format("%-5s %7s %7s %7s %14s %14s\n")
                          % "level"                     // tree level, zero origin
                          % "a-row"                     // actual number of nodes measured at this level
                          % "e-row"                     // estimated number of nodes for this level
                          % "fanout"                    // average fanout for this level
                          % "tot-actual"                // actual total
                          % "tot-estim"                 // estimated total
                          );

    for (size_t i = 0; i < k; ++i) {
        estTotal += estRow;
        if (i < fanout_.size()) {
            // For levels we've seen, use the average fanout
            actualTotal += fanout_[i].second;
            double factor = fanout_[i].first / fanout_[i].second; // average fanout for this level of the tree
            if (debug && i+1 == fanout_.size()) {
                debug <<(boost::format("#%-3d: %7d %7.0f %7.4f %14d %14.0f\n")
                         % i % fanout_[i].second % estRow % factor % actualTotal % estTotal);
            }
            factorTotal += factor;
            estRow *= factor;
        } else if (fanout_.empty()) {
            // If we have no data at all, use a factor of 1.0
        } else {
            // For levels deeper than what we've seen, use the average factor across all levels
            estRow *= factorTotal / fanout_.size();
        }
    }
    SAWYER_MESG(debug) <<(boost::format("average factor = %7.4f\n") % (factorTotal / fanout_.size()));
    return estTotal;
}

// called by managed worker threads, and indirectly by user threads.
void
Engine::doOneStep(const Path::Ptr &path, const BS::RiscOperators::Ptr &ops, const SmtSolver::Ptr &solver) {
    ASSERT_not_null(path);
    ASSERT_forbid(path->isEmpty());
    ASSERT_not_null(ops);

    SAWYER_MESG(mlog[DEBUG]) <<"===== running one step =============================\n";

    // Compute the outgoing state for this path by recursively computing it for each node of the path.
    Sawyer::Stopwatch timer;
    execute(path, ops, solver);

    // Add new work to the queue by extending this path.
    extend(path, ops, solver);

    double seconds = timer.stop();

    // Statistics
    SAWYER_MESG(mlog[DEBUG]) <<"  statistics:\n"
                             <<(boost::format("    time for path step: %1.6f seconds") % seconds) <<"\n"
                             <<"    number of pending work items: " <<nPathsPending() <<"\n"
                             <<"    number of interesing paths:   " <<interesting().size() <<"\n";

    path->lastNode()->incrementProcessingTime(seconds);
}

Sawyer::Stopwatch
Engine::elapsedTime() const {
    SAWYER_THREAD_TRAITS::LockGuard lock(mutex_);
    return elapsedTime_;
}

class PathStatsAccumulator: public PathQueue::Visitor {
public:
    size_t nPaths = 0;                                  // number of paths
    Sawyer::Optional<size_t> minSteps;                  // number of steps in shortest path
    Sawyer::Optional<size_t> maxSteps;                  // number of steps in longest path

    virtual bool operator()(const Path::Ptr &path) {
        ++nPaths;
        size_t nSteps = path->nSteps();
        minSteps = std::min(minSteps.orElse(nSteps), nSteps);
        maxSteps = std::max(maxSteps.orElse(nSteps), nSteps);
        return true;
    }
};

void
Engine::showStatistics(std::ostream &out, const std::string &prefix) const {
    // Gather information
    const size_t k = settings()->k;
    const double forestSize = estimatedForestSize(k);
    const size_t forestExplored = nStepsExplored();
    const double percentExplored = forestSize > 0.0 ? 100.0 * forestExplored / forestSize : 0.0;
    const size_t nPathsExplored = this->nPathsExplored();
    const double age = timeSinceStats_.restart();       // zero if no previous report

    std::vector<InProgress> currentWork = inProgress();
    PathStatsAccumulator currentStats;
    for (const InProgress &work: currentWork)
        currentStats(work.path);

    PathStatsAccumulator pendingStats;
    pendingPaths().traverse(pendingStats);

    ////////////////---------1---------2---------3---------4
    out <<prefix <<"total elapsed time:                     " <<elapsedTime() <<"\n";
    out <<prefix <<"threads:                                " <<nWorking() <<" working of " <<workCapacity() <<" total\n";
    if (currentStats.nPaths > 0) {
        out <<prefix <<"  shortest in-progress path length:     " <<StringUtility::plural(*currentStats.minSteps, "steps") <<"\n";
        out <<prefix <<"  longest in-progress path length:      " <<StringUtility::plural(*currentStats.maxSteps, "steps") <<"\n";
        for (const InProgress &work: currentWork) {
            out <<prefix <<"  thread " <<work.threadId;
            if (work.tid > 0)
                out <<" (LWP " <<work.tid <<")";
            out <<": " <<work.path->printableName()
                <<" having " <<StringUtility::plural(work.path->nSteps(), "steps")
                <<"; " <<work.elapsed <<" elapsed\n";
        }
    }

    out <<prefix <<"paths waiting to be explored:           " <<pendingStats.nPaths <<"\n";
    if (pendingStats.nPaths > 0) {
        out <<prefix <<"  shortest pending path length:         " <<StringUtility::plural(*pendingStats.minSteps, "steps") <<"\n";
        out <<prefix <<"  longest pending path length:          " <<StringUtility::plural(*pendingStats.maxSteps, "steps") <<"\n";
    }
    out <<prefix <<"paths explored:                         " <<nPathsExplored <<"\n";

    const size_t nNewPaths = nPathsExplored - nPathsStats_;
    if (age >= 60.0) {
        double rate = 60.0 * nNewPaths / age;           // paths per minute
         if (nNewPaths >= 1.0) {
            out <<prefix <<(boost::format("%-39s %1.3f paths/minute\n") % "exploration rate:" % rate);
        } else {
            out <<prefix <<(boost::format("%-39s less than one path/minute\n") % "exploration rate:");
        }
    }
    out <<prefix <<"execution tree nodes explored:          " <<forestExplored <<"\n";
    if (forestSize < 1e9) {
        out <<prefix <<(boost::format("%-39s %1.0f nodes estimated\n")
                        % ("execution tree size to depth " + boost::lexical_cast<std::string>(k) + ":") % forestSize);
    } else {
        out <<prefix <<(boost::format("%-39s very large estimated\n")
                        % ("execution tree size to depth " + boost::lexical_cast<std::string>(k) + ":"));
    }
    out <<prefix <<(boost::format("%-39s %1.2f%% estimated\n") % "portion of execution tree explored:" % percentExplored);
    out <<prefix <<"(estimates can be wildly incorrect for small sample sizes)\n";
    if (auto p = std::dynamic_pointer_cast<WorkPredicate>(explorationPredicate())) {
        out <<prefix <<"paths terminated due to K limit:        " <<p->kLimitReached() <<"\n";
        out <<prefix <<"paths terminated due to time limit:     " <<p->timeLimitReached() <<"\n";
    }
    if (auto s = std::dynamic_pointer_cast<P2Model::SemanticCallbacks>(semantics())) {
        out <<prefix <<"paths terminated at duplicate states:   " <<s->nDuplicateStates() <<"\n";
        out <<prefix <<"paths terminated for solver failure:    " <<s->nSolverFailures() <<" (including timeouts)\n";
    }
    out <<prefix <<"symbolic expressions truncated:         " <<nExpressionsTrimmed() <<"\n";

    nPathsStats_ = nPathsExplored;
}

} // namespace
} // namespace
} // namespace

#endif
