#ifndef NETWORKIT_COMMUNITY_PARALLEL_LEIDEN_HPP_
#define NETWORKIT_COMMUNITY_PARALLEL_LEIDEN_HPP_

#include <atomic>
#include <string>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <omp.h>
#include <thread>
#include <tlx/unused.hpp>
#include <networkit/Globals.hpp>
#include <networkit/auxiliary/Parallel.hpp>
#include <networkit/auxiliary/Parallelism.hpp>
#include <networkit/auxiliary/SignalHandling.hpp>
#include <networkit/auxiliary/Timer.hpp>
#include <networkit/coarsening/ParallelPartitionCoarsening.hpp>
#include <networkit/community/CommunityDetectionAlgorithm.hpp>
#include <networkit/community/Modularity.hpp>
#include <networkit/community/ParallelLeidenScoringExtension.hpp>
#include <networkit/community/PLM.hpp>
#include <networkit/graph/Graph.hpp>
#include <networkit/structures/Partition.hpp>

namespace NetworKit {

class ParallelLeiden final : public CommunityDetectionAlgorithm {
public:
    /**
     * @note As reported by Sahu et. al in "GVE-Leiden: Fast Leiden Algorithm for Community
     * Detection in Shared Memory Setting", the current implementation in NetworKit might create a
     * small fraction of disconnected communities. Since this violates the guarantees from the
     * original algorithm, ParallelLeiden should be used with caution. In addition the modularity
     * value of the resulting partition / clustering can be lower compared to other Leiden
     * implementations and even Louvain.
     *
     * @param graph A networkit graph
     * @param iterations Number of Leiden Iterations to be run
     * @param randomize Randomize node order?
     * @param gamma Resolution parameter
     */
    explicit ParallelLeiden(const Graph &graph, int iterations = 3, bool randomize = true,
                            double gamma = 1);

    ~ParallelLeiden() override;

    void run() override;

    /**
     * Load a shared library that customizes the move-phase scoring metric, using the same ABI as
     * ParallelLeidenView (see ParallelLeidenScoringExtension.hpp).  With the CPM extension loaded,
     * this materialising implementation optimises the Constant Potts Model instead of modularity.
     */
    void loadMoveScoringExtension(const std::string &sharedLibraryPath);

    void unloadMoveScoringExtension();

    int VECTOR_OVERSIZE = 10000;

private:
    // Default scoring: modularity.  Signatures and semantics match ParallelLeidenView so that one
    // scoring extension (for instance ParallelLeidenCPMScoringExtension) serves both classes.
    static double modularityCommunityScore(double cutD, double degreeV, double volD,
                                           count subsetSize, count sizeD, double gamma,
                                           double inverseGraphVolume) {
        tlx::unused(subsetSize);
        tlx::unused(sizeD);
        return cutD - gamma * degreeV * volD * inverseGraphVolume;
    }

    static double modularityThresholdScore(double cutC, double degreeV, double volC,
                                           count subsetSize, count sizeC, double gamma,
                                           double inverseGraphVolume) {
        tlx::unused(subsetSize);
        tlx::unused(sizeC);
        return cutC - gamma * (volC - degreeV) * degreeV * inverseGraphVolume;
    }

    static bool modularityRefineRSetCondition(double cutWeight, double subsetVolume,
                                              count subsetSize, double targetVolume,
                                              count targetSize, double sourceVolume,
                                              count sourceSize, double gamma,
                                              double inverseGraphVolume) {
        tlx::unused(subsetSize);
        tlx::unused(targetSize);
        tlx::unused(sourceVolume);
        tlx::unused(sourceSize);
        return cutWeight >= gamma * subsetVolume * targetVolume * inverseGraphVolume;
    }

    static bool modularityRefineTSetCondition(double cutWeight, double subsetVolume,
                                              count subsetSize, double targetVolume,
                                              count targetSize, double sourceVolume,
                                              count sourceSize, double gamma,
                                              double inverseGraphVolume) {
        tlx::unused(subsetSize);
        tlx::unused(targetSize);
        tlx::unused(sourceVolume);
        tlx::unused(sourceSize);
        return cutWeight >= gamma * subsetVolume * targetVolume * inverseGraphVolume;
    }

    inline double scoreCommunity(double cutWeight, double degree, double communityVolume,
                                 count subsetSize, count communitySize) const {
        return communityScoreFunction_(cutWeight, degree, communityVolume, subsetSize,
                                       communitySize, gamma, inverseGraphVolume);
    }

    inline double scoreCurrentCommunityThreshold(double cutWeight, double degree,
                                                 double communityVolume, count subsetSize,
                                                 count communitySize) const {
        return currentCommunityThresholdFunction_(cutWeight, degree, communityVolume, subsetSize,
                                                  communitySize, gamma, inverseGraphVolume);
    }

    inline bool refineRSetCondition(double cutWeight, double subsetVolume, count subsetSize,
                                    double targetVolume, count targetSize, double sourceVolume,
                                    count sourceSize) const {
        return refineRSetConditionFunction_(cutWeight, subsetVolume, subsetSize, targetVolume,
                                            targetSize, sourceVolume, sourceSize, gamma,
                                            inverseGraphVolume);
    }

    inline bool refineTSetCondition(double cutWeight, double subsetVolume, count subsetSize,
                                    double targetVolume, count targetSize, double sourceVolume,
                                    count sourceSize) const {
        return refineTSetConditionFunction_(cutWeight, subsetVolume, subsetSize, targetVolume,
                                            targetSize, sourceVolume, sourceSize, gamma,
                                            inverseGraphVolume);
    }

    static inline void lockLowerFirst(index a, index b, std::vector<std::mutex> &locks) {
        if (a < b) {
            locks[a].lock();
            locks[b].lock();
        } else {
            locks[b].lock();
            locks[a].lock();
        }
    }

    void flattenPartition();

    void calculateVolumes(const Graph &graph);

    void parallelMove(const Graph &graph);

    Partition parallelRefine(const Graph &graph);

    double inverseGraphVolume; // 1/vol(V)

    std::vector<double> communityVolumes;

    // Number of original nodes in each community, and in each node of the current level.  Needed by
    // size-based objectives such as CPM; unused by modularity.
    std::vector<count> communitySizes;
    std::vector<count> nodeSizes;

    std::vector<std::vector<node>> mappings;

    static constexpr int WORKING_SIZE = 1000;

    double gamma; // Resolution parameter

    bool changed;

    int numberOfIterations;

    Aux::SignalHandler handler;

    bool random;

    void *scoringExtensionHandle_ = nullptr;
    ParallelLeidenCommunityScoreFunction communityScoreFunction_ = &modularityCommunityScore;
    ParallelLeidenCommunityScoreFunction currentCommunityThresholdFunction_ =
        &modularityThresholdScore;
    ParallelLeidenRefineSetConditionFunction refineRSetConditionFunction_ =
        &modularityRefineRSetCondition;
    ParallelLeidenRefineSetConditionFunction refineTSetConditionFunction_ =
        &modularityRefineTSetCondition;
    std::string scoringExtensionPath_;
};

} // namespace NetworKit

#endif // NETWORKIT_COMMUNITY_PARALLEL_LEIDEN_HPP_
