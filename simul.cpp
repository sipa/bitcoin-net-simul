#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <vector>
#include <set>
#include <map>
#include <memory>
#include <thread>

struct Miner;

/** How many computation threads to use. */
static const int CONCURRENCY = 8;

/** Bitcoin constants. */
static const double RETARGET_TIME = 14 * 24 * 60 * 60;
static const int RETARGET_BLOCKS = 2016;
static const double INTERVAL_TIME = RETARGET_TIME / RETARGET_BLOCKS;
static const int SUBSIDY = 25;

/** Helper functions for block skiplist. */
int static inline InvertLowestOne(int n) { return n & (n - 1); }
int static inline GetSkipHeight(int height) {
    if (height < 2)
        return 0;
    return std::max(height - 1008, (height & 1) ? InvertLowestOne(InvertLowestOne(height - 1)) + 1 : InvertLowestOne(height));
}

struct Block;

double CalculateNewDifficulty(double time, Block* prev);

/** Block structure. */
struct Block {
    const int size;
    const int owner;
    Block* const prev;
    const int height;
    Block* const skip;
    const double time;
    const double difficulty;
    const double work;
    const double value;

private:

public:
    /** Get an ancestor of this block at the specified height. */
    Block* GetAncestor(int height) {
        if (height > this->height || height < 0)
            return NULL;

        Block* indexWalk = this;
        int heightWalk = this->height;
        while (heightWalk > height) {
            int heightSkip = GetSkipHeight(heightWalk);
        int heightSkipPrev = GetSkipHeight(heightWalk - 1);
        if (indexWalk->skip != NULL &&
            (heightSkip == height ||
             (heightSkip > height && !(heightSkipPrev < heightSkip - 2 &&
                                       heightSkipPrev >= height)))) {
                indexWalk = indexWalk->skip;
                heightWalk = heightSkip;
            } else {
                indexWalk = indexWalk->prev;
                heightWalk--;
            }
        }
        return indexWalk;
    }

    /** Construct a non-genesis block. */
    Block(int size_, double time_, Block* prev_, int owner_, double value_)
        : size(size_), owner(owner_), prev(prev_), height(prev->height + 1),
          skip(prev->GetAncestor(GetSkipHeight(height))),
          time(time_), difficulty(CalculateNewDifficulty(time_, prev_)),
          work(prev->work + prev->difficulty), value(value_) {}

    /* Construct a genesis block. */
    Block(double diff)
        : size(0), owner(-1), prev(NULL), height(0), skip(NULL), time(0), difficulty(diff), work(0), value(0) {}
};

double CalculateNewDifficulty(double time, Block* prev) {
    if (prev == NULL) return 1.0;
    if ((prev->height + 1) % RETARGET_BLOCKS != 0) {
        return prev->difficulty;
    }
    Block* first = prev->GetAncestor(std::max(prev->height + 1 - RETARGET_BLOCKS, 0));
    double timespan = prev->time - first->time;
    if (timespan < RETARGET_TIME * 0.25)
        timespan = RETARGET_TIME * 0.25;
    if (timespan > RETARGET_TIME * 4.0)
        timespan = RETARGET_TIME * 4.0;
    double newdiff = prev->difficulty * RETARGET_TIME / timespan;
    if (newdiff < 1.0)
        newdiff = 1.0;
    return newdiff;
}

struct Simulation;

enum EventType {
    /* A block 'data' was received through the network. */
    EVENT_TYPE_RECEIVED_BLOCK,

    /* A block 'data' is done processing. */
    EVENT_TYPE_PROCESSED_BLOCK,

    /** A block was mined. The 'data' passed is the best block at the time the
     *  mining process was started. For performance reasons, it is only changed
     *  when the difficulty changes, so this is not necessarily the actual parent
     * block of the block being mined.
     */
    EVENT_TYPE_MINED,
};

/** Abstract base class for all classes that receive block events. */
struct EventHandler {
    void virtual HandleEvent(Simulation* simulation, double time, EventType type, Block* data) const = 0;
};

/** Encapsulated event. */
struct Event {
    const EventHandler* handler;
    EventType type;
    Block* data; /** See the description in EventType for the meaning of this field. */
    Event(const EventHandler* h, EventType t, Block* d) : handler(h), type(t), data(d) {}
};

class Node;
class Miner;

/** The entire state of a simulation. The network data structures (Node, Miner,
 *  Link) are const during simulation to allow multithreading (where each thread
 *  has its own Simulation object).
 */
struct Simulation {
    /** The configured fee per byte for created blocks. */
    double fee_per_byte;

    /** The queue of events. */
    std::multimap<double, Event> events;

    /** Current time in the simulation. */
    double current_time;

    /** All blocks created (and owned by) this simulation. */
    std::vector<std::unique_ptr<Block> > blocks;

    /** Which blocks are known to which nodes (to avoid propagation looping forever).
     *  Note that old blocks are removed from this, to preserve memory. */
    std::set<std::pair<const Node*, Block*> > block_known;

    /** The best block known to each miner (only updated after processing it).
     *  This is the block newly mined blocks will have a parent. */
    std::map<const Miner*, Block*> block_best;

    /** The best block overall. */
    Block* best_block;
    /** How much income each of the mining groups have had at best_block. */
    std::map<int, double> best_accounts;
    /** How much income all mining groups together have had at best_block. */
    double total_accounts;

    /** Sum of the sizes of all blocks in the best_block chain. */
    double total_blocksize;

    /** Run a single event. */
    bool RunEvent(double deadline) {
        if (events.empty()) return false;
        std::multimap<double, Event>::iterator b = events.begin();
        if (b->first > deadline) return false;
        current_time = b->first;
        b->second.handler->HandleEvent(this, b->first, b->second.type, b->second.data);
        events.erase(b);
        return true;
    }

    /** Run until deadline. */
    void Run(double deadline) {
        while (RunEvent(deadline)) {}
    }

    /** Add an event to the queue. */
    void AddEvent(double ts, const EventHandler* handler, EventType type, Block* data) {
        assert(ts >= current_time);
        events.insert(std::make_pair(ts, Event(handler, type, data)));
    }

    /** Mine a new block. */
    Block* CreateBlock(int size, double time, Block* prev, int owner) {
        blocks.push_back(std::unique_ptr<Block>(new Block(size, time, prev, owner, SUBSIDY + fee_per_byte * size)));
        Block* block = blocks.back().get();

        if (block->work > best_block->work) {
            while (block->GetAncestor(best_block->height) != best_block) {
                 best_accounts[best_block->owner] -= best_block->value;
                 total_accounts -= best_block->value;
                 total_blocksize += best_block->size;
                 best_block = best_block->prev;
            }
            while (best_block != block) {
                 best_block = block->GetAncestor(best_block->height + 1);
                 total_accounts += best_block->value;
                 total_blocksize -= best_block->size;
                 best_accounts[best_block->owner] += best_block->value;
            }
        }
        return block;
    }

    Simulation(double fee_per_byte_, double difficulty, const std::vector<std::unique_ptr<Node> >& nodes);
};

/** Abstract base class of entities that are able to receive a block. */
class BlockReceiver : public EventHandler {
public:
    virtual void ReceivedBlock(Simulation* simul, double time, Block* block) const = 0;
    void HandleEvent(Simulation* simul, double time, EventType type, Block* data) const {
        if (type == EVENT_TYPE_RECEIVED_BLOCK) {
            ReceivedBlock(simul, time, data);
        }
    }
};

class Node;

/** Network connection between two nodes. */
struct Link {
    const Node* side_a;
    const Node* side_b;
    double delay;
    double delay_per_byte;

    Link(Node* a, Node* b, double latency_, double megabit_per_second_);
};

/** A full node in the network that processes blocks and relays them. */
class Node : public BlockReceiver {
protected:
    double delay;
    double delay_per_byte;
    std::vector<Link*> destinations;

public:
    /** When receiving a block, start processing it if we didn't have it already. */
    void ReceivedBlock(Simulation* simul, double time, Block* block) const {
        std::pair<const Node*,Block*> newknown(this, block);
        std::pair<std::set<std::pair<const Node*,Block*> >::iterator, bool> ret = simul->block_known.insert(newknown);
        if (ret.second) {
            simul->AddEvent(time + delay + delay_per_byte * block->size, this, EVENT_TYPE_PROCESSED_BLOCK, block);
        }
        std::pair<const Node*,Block*> oldknown(this, block->GetAncestor(block->height - RETARGET_TIME));
        simul->block_known.erase(oldknown);
    }

    void HandleEvent(Simulation* simul, double time, EventType type, Block* data) const {
        if (type == EVENT_TYPE_PROCESSED_BLOCK) {
            ProcessedBlock(time, data, simul);
        }
        BlockReceiver::HandleEvent(simul, time, type, data);
    }

    /** When a block is done processing, relay it. */
    virtual void ProcessedBlock(double time, Block* block, Simulation* simul) const {
        for (size_t i = 0; i < destinations.size(); i++) {
            const Link* link = destinations[i];
            const Node* peer = NULL;
            if (link->side_a == this) {
                peer = link->side_b;
            } else {
                peer = link->side_a;
            }
            simul->AddEvent(time + link->delay + link->delay_per_byte * block->size, peer, EVENT_TYPE_RECEIVED_BLOCK, block);
        }
    }

    virtual void Initialize(Simulation* simul) const {
    }

    Node(double delay_, double delay_per_byte_) : delay(delay_), delay_per_byte(delay_per_byte_) {}

    friend class Link;
};

Simulation::Simulation(double fee_per_byte_, double difficulty, const std::vector<std::unique_ptr<Node> >& nodes) : fee_per_byte(fee_per_byte_) {
    blocks.push_back(std::unique_ptr<Block>(new Block(difficulty)));
    best_block = blocks.back().get();
    current_time = 0;
    for (size_t n = 0; n < nodes.size(); n++) {
        nodes[n]->Initialize(this);
    }
}

Link::Link(Node* a, Node* b, double delay_, double delay_per_byte_) : side_a(a), side_b(b), delay(delay_), delay_per_byte(delay_per_byte_) {
    a->destinations.push_back(this);
    b->destinations.push_back(this);
};

class Miner : public Node {
protected:
    double hashrate;
    int owner;
    double mining_delay;

public:
    int block_size;

    void HandleEvent(Simulation* simul, double time, EventType type, Block* data) const {
        if (type == EVENT_TYPE_MINED) {
            /* If the difficulty didn't change in between, actually create a
             * block. Otherwise, a new event for the new difficulty will have
             * been scheduled already, and we can ignore this.
             */
            if (data->difficulty == simul->block_best[this]->difficulty) {
                Block* newblock = simul->CreateBlock(block_size, time, simul->block_best[this], owner);
                ReceivedBlock(simul, time, newblock);
                ScheduleBlockMine(time, simul);
            }
        }
        Node::HandleEvent(simul, time, type, data);
    }

    void ScheduleBlockMine(double time, Simulation* simul) const {
        long double uniform = (random() + 0.5l) / (((long double)RAND_MAX) + 1);
        /* -log(U) / delta produces values with exponential distibution, modelling the times between events in a Poisson process with rate delta. */
        simul->AddEvent(time + mining_delay - logl(uniform) * INTERVAL_TIME * simul->block_best[this]->difficulty / hashrate, this, EVENT_TYPE_MINED, simul->block_best[this]);
    }

    void ProcessedBlock(double time, Block* block, Simulation* simul) const {
        /* When a miner processes a block, check whether it's our new best, and
         * potentially reschedule if it has new difficulty.
         */
        if (block->work > simul->block_best[this]->work) {
            double olddiff = simul->block_best[this]->difficulty;
            simul->block_best[this] = block;
            if (olddiff != block->difficulty) {
                ScheduleBlockMine(time, simul);
            }
        }
        Node::ProcessedBlock(time, block, simul);
    }

    void Initialize(Simulation* simul) const {
        /* When a miner is started, also start its block mining event schedule. */
        Node::Initialize(simul);
        simul->block_best[this] = simul->best_block;
        ScheduleBlockMine(simul->current_time, simul);
    }

    Miner(double delay_, double delay_per_byte_, double mining_delay_, double h, int b, int o) : Node(delay_, delay_per_byte_), mining_delay(mining_delay_), hashrate(h), block_size(b), owner(o) {}
};

struct Network {
    std::vector<std::unique_ptr<Node> > nodes;
    std::vector<std::unique_ptr<Link> > links;
    std::map<int, double> hashrate_per_account;
    std::map<int, double> hashrate_times_blocksize_per_account;
    double total_hashrate;
    double total_hashrate_times_blocksize;
    double average_fee_per_block;

    Network(double average_fee_per_block_) : total_hashrate(0), total_hashrate_times_blocksize(0), average_fee_per_block(average_fee_per_block_) {}

    int AddMiner(double delay_ms_, double processing_megabit_per_s_, double mining_delay_ms_, double hashrate_, int blocksize, int owner) {
        nodes.push_back(std::unique_ptr<Node>(new Miner(delay_ms_ * 0.001, 0.000008 / processing_megabit_per_s_, mining_delay_ms_ * 0.001, hashrate_, blocksize, owner)));
        hashrate_per_account[owner] += hashrate_;
        hashrate_times_blocksize_per_account[owner] += hashrate_ * blocksize;
        total_hashrate += hashrate_;
        total_hashrate_times_blocksize += hashrate_ * blocksize;
        return nodes.size();
    }

    int AddNode(double delay_ms_, double processing_megabit_per_s_) {
        nodes.push_back(std::unique_ptr<Node>(new Node(delay_ms_ * 0.001, 0.000008 / processing_megabit_per_s_)));
        return nodes.size();
    }

    void AddLink(int node1, int node2, double latency_in_ms, double megabit_per_s) {
        links.push_back(std::unique_ptr<Link>(new Link(nodes[node1].get(), nodes[node2].get(), latency_in_ms * 0.001, 0.000008 / megabit_per_s)));
    }
};

void RunSimulation(const struct Network* net, double period, std::map<int, double>* ret) {
    double average_blocksize = net->total_hashrate_times_blocksize / net->total_hashrate;
    double fee_per_byte = net->average_fee_per_block / average_blocksize;
    Simulation sim(fee_per_byte, net->total_hashrate, net->nodes);
    sim.Run(period);
    *ret = sim.best_accounts;
    double total = 0;
    for (std::pair<const int, double>& key : *ret) {
        key.second /= sim.total_accounts;
    }
}

void RunSimulations(struct Network* net, double period, double accuracy, std::map<int, double>& ret) {
    int concur = CONCURRENCY;

    std::vector<std::map<int, double> > rets;
    int n = 0;
    do {
        rets.resize((n + 1) * concur);
        std::thread threads[concur];
        for (int i = 0; i < concur; i++) {
            std::map<int, double>* ret = &rets[n * concur + i];
            threads[i] = std::thread(RunSimulation, net, period, ret);
        }
        for (int i = 0; i < concur; i++) {
            threads[i].join();
        }

        bool cont = n < 3;
        for (const std::pair<int, double>& key : rets[0]) {
            double sum = 0;
            int count = (n + 1) * concur;
            for (size_t i = 0; i < count; i++) {
                sum += rets[i][key.first];
            }
            double avg = sum / count;
            double qsum = 0;
            for (size_t i = 0; i < count; i++) {
                qsum += (rets[i][key.first] - avg) * (rets[i][key.first] - avg);
            }
            double variance = qsum / (count - 1) / count;
            if (variance > avg * avg * accuracy * accuracy) {
                cont = true;
                break;
            }
            ret[key.first] = avg;
        }
        n++;
        if (!cont) break;
    } while(true);
}


void TryBlockSize(int size_a, int size_b, double total_fees_per_block) {
    Network net(total_fees_per_block);

    // Three fast miners, with 25%, 25% and 30% of the hashrate with 500ms mining delay, connected via gigabit/s and 5ms between them, processing blocks at 50 Mbit/s.
    net.AddMiner(50, 50, 500, 2500, size_a, 0);
    net.AddMiner(50, 50, 500, 2500, size_a, 0);
    net.AddMiner(50, 50, 500, 3000, size_a, 0);
    net.AddLink(0, 1, 5, 1000);
    net.AddLink(1, 2, 5, 1000);
    net.AddLink(2, 0, 5, 1000);

    // Four small miners, with 5% hashrate each, with 100 Mbit/s links between them, processing blocks at 20 Mbit/s.
    net.AddMiner(50, 50, 500, 500, size_b, 1);
    net.AddMiner(50, 50, 500, 500, size_b, 1);
    net.AddMiner(50, 50, 500, 500, size_b, 1);
    net.AddMiner(50, 50, 500, 500, size_b, 1);
    net.AddLink(3, 4, 5, 1000);
    net.AddLink(3, 5, 5, 1000);
    net.AddLink(3, 6, 5, 1000);
    net.AddLink(4, 5, 5, 1000);
    net.AddLink(4, 6, 5, 1000);
    net.AddLink(5, 6, 5, 1000);

    // The two groups connected with 50 Mbit/s and 300 ms latency.
    net.AddLink(3, 0, 300, 100);

    std::map<int, double> ret;

    // Simulate 2 months of time, with allowed relative standard deviation 0.05%
    printf("Configuration:\n");
    for (const std::pair<int, double>& key : net.hashrate_per_account) {
        printf("  * Miner group %i: %f%% hashrate, blocksize %f\n", key.first, key.second / net.total_hashrate * 100, net.hashrate_times_blocksize_per_account[key.first] / net.hashrate_per_account[key.first]);
    }
    printf("  * Expected average block size: %f\n", net.total_hashrate_times_blocksize / net.total_hashrate);
    printf("  * Average fee per block: %f\n", net.average_fee_per_block);
    printf("  * Fee per byte: %.10f\n", total_fees_per_block / net.total_hashrate_times_blocksize * net.total_hashrate);
    RunSimulations(&net, 86400 * 365 / 6, 0.0005 / 12, ret);
    printf("Result:\n");
    for (const std::pair<int, double>& key : ret) {
        printf("  * Miner group %i: %f%% income (factor %f with hashrate)\n", key.first, key.second * 100, key.second / (net.hashrate_per_account[key.first]) * net.total_hashrate);
    }
    printf("\n");
}

int main(void) {
    double fees_per_block[] = {0.25, 25};
    int sizes_a[] = {20000000};
    int sizes_b[] = {1000000, 20000000};
    for (double fee_per_block : fees_per_block) {
        for (int size_a : sizes_a) {
            for (int size_b : sizes_b) {
                TryBlockSize(size_a, size_b, fee_per_block);
            }
        }
    }
    return 0;
}
