#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <vector>
#include <set>
#include <map>
#include <memory>
#include <thread>

struct Miner;

static const int CONCURRENCY = 8;
static const double RETARGET_TIME = 14 * 24 * 60 * 60;
static const int RETARGET_BLOCKS = 2016;
static const double INTERVAL_TIME = RETARGET_TIME / RETARGET_BLOCKS;
static const int SUBSIDY = 25;

int static inline InvertLowestOne(int n) { return n & (n - 1); }
int static inline GetSkipHeight(int height) {
    if (height < 2)
        return 0;
    return std::max(height - 1008, (height & 1) ? InvertLowestOne(InvertLowestOne(height - 1)) + 1 : InvertLowestOne(height));
}

struct Block;

double CalculateNewDifficulty(double time, Block* prev);

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

    Block(int size_, double time_, Block* prev_, int owner_, double value_)
        : size(size_), owner(owner_), prev(prev_), height(prev->height + 1),
          skip(prev->GetAncestor(GetSkipHeight(height))),
          time(time_), difficulty(CalculateNewDifficulty(time_, prev_)),
          work(prev->work + prev->difficulty), value(value_) {}

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
    EVENT_TYPE_RECEIVED_BLOCK,
    EVENT_TYPE_PROCESSED_BLOCK,
    EVENT_TYPE_MINED,
};

struct EventHandler {
    void virtual HandleEvent(Simulation* simulation, double time, EventType type, Block* data) const {
    }
};

struct Event {
    const EventHandler* handler;
    EventType type;
    Block* data;
    Event(const EventHandler* h, EventType t, Block* d) : handler(h), type(t), data(d) {}
};

class Node;
class Miner;

struct Simulation {
    double fee_per_byte;

    std::multimap<double, Event> events;
    double current_time;

    std::vector<std::unique_ptr<Block> > blocks;

    std::set<std::pair<const Node*, Block*> > block_known;
    std::map<const Miner*, Block*> block_best;

    Block* best_block;
    std::map<int, double> best_accounts;
    double total_accounts;

    bool RunEvent(double deadline) {
        if (events.empty()) return false;
        std::multimap<double, Event>::iterator b = events.begin();
        if (b->first > deadline) return false;
        current_time = b->first;
        b->second.handler->HandleEvent(this, b->first, b->second.type, b->second.data);
        events.erase(b);
        return true;
    }

    void Run(double deadline) {
        while (RunEvent(deadline)) {}
    }

    void AddEvent(double ts, const EventHandler* handler, EventType type, Block* data) {
        assert(ts >= current_time);
        events.insert(std::make_pair(ts, Event(handler, type, data)));
    }

    Block* CreateBlock(int size, double time, Block* prev, int owner) {
        blocks.push_back(std::unique_ptr<Block>(new Block(size, time, prev, owner, SUBSIDY + fee_per_byte * size)));
        Block* block = blocks.back().get();

        if (block->work > best_block->work) {
            while (block->GetAncestor(best_block->height) != best_block) {
                 best_accounts[best_block->owner] -= best_block->value;
                 total_accounts -= best_block->value;
                 best_block = best_block->prev;
            }
            while (best_block != block) {
                 best_block = block->GetAncestor(best_block->height + 1);
                 total_accounts += best_block->value;
                 best_accounts[best_block->owner] += best_block->value;
            }
        }
        return block;
    }

    Simulation(double fee_per_byte_, double difficulty, const std::vector<std::unique_ptr<Node> >& nodes);
};

class BlockReceiver : public EventHandler {
public:
    virtual void ReceivedBlock(Simulation* simul, double time, Block* block) const = 0;
    void HandleEvent(Simulation* simul, double time, EventType type, Block* data) const {
        if (type == EVENT_TYPE_RECEIVED_BLOCK) {
            ReceivedBlock(simul, time, data);
        }
        EventHandler::HandleEvent(simul, time, type, data);
    }
};

class Node;

class Link {
public:
    const Node* side_a;
    const Node* side_b;
private:
    double delay;
    double delay_per_byte;
    double static_packet_loss;

public:
    Link(Node* a, Node* b, double latency_, double megabit_per_second_, double static_packet_loss_);

    double getDelay(int size) const {
        return delay + delay_per_byte * size;
    }
};

class Node : public BlockReceiver {
protected:
    double delay;
    double delay_per_byte;
    std::vector<Link*> destinations;

public:
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

    virtual void ProcessedBlock(double time, Block* block, Simulation* simul) const {
        for (size_t i = 0; i < destinations.size(); i++) {
            const Link* link = destinations[i];
            const Node* peer = NULL;
            if (link->side_a == this) {
                peer = link->side_b;
            } else {
                peer = link->side_a;
            }
            simul->AddEvent(time + link->getDelay(block->size), peer, EVENT_TYPE_RECEIVED_BLOCK, block);
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

Link::Link(Node* a, Node* b, double delay_, double delay_per_byte_, double static_packet_loss_) : side_a(a), side_b(b), delay(delay_), delay_per_byte(delay_per_byte_), static_packet_loss(static_packet_loss_) {
    a->destinations.push_back(this);
    b->destinations.push_back(this);
};

class Miner : public Node {
protected:
    double hashrate;
    int block_size;
    int owner;
    double mining_delay;

public:
    void HandleEvent(Simulation* simul, double time, EventType type, Block* data) const {
        if (type == EVENT_TYPE_MINED) {
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
        simul->AddEvent(time + mining_delay - logl(uniform) * INTERVAL_TIME * simul->block_best[this]->difficulty / hashrate, this, EVENT_TYPE_MINED, simul->block_best[this]);
    }

    void ProcessedBlock(double time, Block* block, Simulation* simul) const {
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
    double total_hashrate;

public:
    Network() : total_hashrate(0) {}

    int AddMiner(double delay_ms_, double processing_megabit_per_s_, double mining_delay_ms_, double hashrate_, int blocksize, int owner) {
        nodes.push_back(std::unique_ptr<Node>(new Miner(delay_ms_ * 0.001, 0.000008 / processing_megabit_per_s_, mining_delay_ms_ * 0.001, hashrate_, blocksize, owner)));
        hashrate_per_account[owner] += hashrate_;
        total_hashrate += hashrate_;
        return nodes.size();
    }

    int AddNode(double delay_ms_, double processing_megabit_per_s_) {
        nodes.push_back(std::unique_ptr<Node>(new Node(delay_ms_ * 0.001, 0.000008 / processing_megabit_per_s_)));
        return nodes.size();
    }

    void AddLink(int node1, int node2, double latency_in_ms, double megabit_per_s, double static_packet_loss) {
        links.push_back(std::unique_ptr<Link>(new Link(nodes[node1].get(), nodes[node2].get(), latency_in_ms * 0.001, 0.000008 / megabit_per_s, static_packet_loss)));
    }
};

void RunSimulation(const struct Network* net, double fee_per_kilobyte, double period, std::map<int, double>* ret) {
    Simulation sim(fee_per_kilobyte * 0.001, net->total_hashrate, net->nodes);
    sim.Run(period);
    *ret = sim.best_accounts;
    double total = 0;
    for (std::pair<const int, double>& key : *ret) {
        key.second /= sim.total_accounts;
    }
}

void RunSimulations(struct Network* net, double fee_per_kilobyte, double period, double accuracy, std::map<int, double>& ret) {
    int concur = CONCURRENCY;

    std::vector<std::map<int, double> > rets;
    int n = 0;
    do {
        rets.resize((n + 1) * concur);
        std::thread threads[concur];
        for (int i = 0; i < concur; i++) {
            std::map<int, double>* ret = &rets[n * concur + i];
            threads[i] = std::thread(RunSimulation, net, fee_per_kilobyte, period, ret);
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

int main(void) {
    Network net;

    // Three fast miners, with 25%, 25% and 30% of the hashrate with 500ms mining delay, connected via gigabit/s and 5ms between them.
    net.AddMiner(50, 50, 500, 2500, 200000000, 0);
    net.AddMiner(50, 50, 500, 2500, 200000000, 0);
    net.AddMiner(50, 50, 500, 3000, 200000000, 0);
    net.AddLink(0, 1, 5, 1000, 0);
    net.AddLink(1, 2, 5, 1000, 0);
    net.AddLink(2, 0, 5, 1000, 0);

    // Four small miners, with 5% hashrate each, with a few 100 Mbit/s links between them.
    net.AddMiner(100, 30, 2000, 500, 1200000, 1);
    net.AddMiner(100, 30, 2000, 500, 1200000, 1);
    net.AddMiner(100, 30, 2000, 500, 1200000, 1);
    net.AddMiner(100, 30, 2000, 500, 1200000, 1);
    net.AddLink(3, 4, 200, 100, 0);
    net.AddLink(3, 5, 200, 100, 0);
    net.AddLink(3, 6, 200, 100, 0);
    net.AddLink(4, 5, 200, 100, 0);
    net.AddLink(4, 6, 200, 100, 0);
    net.AddLink(5, 6, 200, 100, 0);

    // Each of the small miners connected to two fast miners, with 20 Mbit/s and 300 ms latency.
    net.AddLink(3, 0, 300, 20, 0.1);

    std::map<int, double> ret;
    RunSimulations(&net, 0.00001000, 86400 * 365, 0.005, ret);
    for (const std::pair<int, double>& key : ret) {
        printf("Account %i: %f%%\n", key.first, key.second * 100);
    }
    return 0;
}
