#include <iostream>
#include <chrono>
#include <limits>

#include "Server.h"
#include "ORBmatcher.h"

using namespace std::literals::chrono_literals;

namespace ORB_SLAM2 {

constexpr int SEQA = 0;
constexpr int SEQB = 1;


Server::Server(const std::string &configFile, const string &strVocFile) : stopped(false) {
    std::cout << "Starting server..." << std::endl;

    config = YAML::LoadFile(configFile);

    std::cout <<  "Loading ORB Vocabulary. This could take a while..." << std::endl;

    vocabulary = new ORBVocabulary();

    const std::string txt_suffix(".txt"); 
    bool loaded;
    if (strVocFile.find(txt_suffix, strVocFile.size() - txt_suffix.size()) != string::npos)
        loaded = vocabulary->loadFromTextFile(strVocFile);
    else
        loaded = vocabulary->loadFromBinaryFile(strVocFile);
    if(!loaded)
    {
        cerr << "Wrong path to vocabulary. " << endl;
        cerr << "Falied to open at: " << strVocFile << endl;
        exit(-1);
    }
    cout << "Vocabulary loaded!" << endl << endl;


    // Create viewer, but don't start until global mapping
    globalMap = new Map();
    mapDrawer = new MapDrawer(globalMap, configFile);
    viewer = new Viewer(nullptr, this, nullptr, mapDrawer, nullptr, configFile, "Server");

    std::cout << "Server started" << std::endl;
}

void Server::Shutdown() {
    std::cout << "Shutting down server..." << std::endl;

    stopped = true;

    std::this_thread::sleep_for(100ms);

    std::cout << "Server shut down." << std::endl;
}

void Server::Run() {
    std::cout << "Running server" << std::endl;

    bool monocular = config["monocular"].as<bool>();

    keyFrameDictionary[nullptr] = nullptr;
    mapPointDictionary[nullptr] = nullptr;

    globalDatabase = new KeyFrameDatabase(*vocabulary);
    globalLoopClosing = new GlobalLoopClosing(globalMap, globalDatabase, vocabulary, !monocular);

    // // Start global mapping and viewer
    viewerThread = new std::thread(&Viewer::Run, viewer);

    std::vector<KeyFrame*> keyframesSeqA = clients[0]->mpMap->GetAllKeyFrames();
    std::vector<KeyFrame*> keyframesSeqB = clients[1]->mpMap->GetAllKeyFrames();

    std::vector<MapPoint*> mappointsSeqA = clients[0]->mpMap->GetAllMapPoints();
    std::vector<MapPoint*> mappointsSeqB = clients[1]->mpMap->GetAllMapPoints();

    std::cout << "Ready to insert items..." << std::endl;
    cin.ignore();

    // while(!stopped) {
    for(KeyFrame* keyframe : keyframesSeqA) {
        InsertNewKeyFrame(keyframe, 0, SEQA);
        std::this_thread::sleep_for(25ms);
    }

    for(KeyFrame* keyframe : keyframesSeqA) {
        CopyKeyFrameMappoints(keyframe);
        std::this_thread::sleep_for(25ms);
    }

    for(KeyFrame* keyframe : keyframesSeqA) {
        CopyKeyFrameConnections(keyframe);
        std::this_thread::sleep_for(25ms);
    }

    int offset = globalMap->GetAllKeyFrames().size();
    for(KeyFrame* keyframe : keyframesSeqB) {
        InsertNewKeyFrame(keyframe, offset, SEQB);
        std::this_thread::sleep_for(25ms);
    }

    for(KeyFrame* keyframe : keyframesSeqB) {
        CopyKeyFrameMappoints(keyframe);
        std::this_thread::sleep_for(25ms);
    }

    for(KeyFrame* keyframe : keyframesSeqB) {
        CopyKeyFrameConnections(keyframe);
        std::this_thread::sleep_for(25ms);
    }

    std::cout << "adding april tag connections" << std::endl;

    FindAprilTagConnections();

    std::cout << "Finished inserting items. Press enter to optimize..." << std::endl;
    cin.ignore();

    globalMappingThread = new std::thread(&GlobalLoopClosing::Run, globalLoopClosing);
    
    for(KeyFrame* keyframe : globalMap->GetAllKeyFrames()) {
        globalLoopClosing->InsertKeyFrame(keyframe);
        std::this_thread::sleep_for(50ms);
    }

    std::cout << "Press enter to run final global bundle adjustment..." << std::endl;
    cin.ignore();

    globalLoopClosing->RunGlobalBundleAdjustment(0);

}

void Server::InsertNewKeyFrame(KeyFrame* keyframe, int offset, int sequence) {
    KeyFrame* newKeyFrame = new KeyFrame(keyframe, globalMap, globalDatabase);
    newKeyFrame->mnId += offset;
    newKeyFrame->sequence = sequence;

    if(sequence == SEQA) {
        timeDictionaryA[newKeyFrame->mTimeStamp] = newKeyFrame;
    } else {
        timeDictionaryB[newKeyFrame->mTimeStamp] = newKeyFrame;
    }

    newKeyFrame->ComputeBoW();
    globalMap->AddKeyFrame(newKeyFrame);
    keyFrameDictionary[keyframe] = newKeyFrame;
}

void Server::CopyKeyFrameMappoints(KeyFrame* keyframe) {
    KeyFrame* globalKeyFrame = keyFrameDictionary[keyframe];
    globalKeyFrame->mvpMapPoints.resize(keyframe->GetMapPointMatches().size());
    // Transfer map points
    size_t i = 0;
    for(MapPoint* point : keyframe->mvpMapPoints) {
        if(mapPointDictionary.find(point) == mapPointDictionary.end()) {
            // Point not in server map
            MapPoint* newMapPoint = new MapPoint(point->GetWorldPos(), globalKeyFrame, globalMap);
            newMapPoint->mDescriptor = point->mDescriptor.clone();
            newMapPoint->AddObservation(globalKeyFrame,i);
            globalKeyFrame->AddMapPoint(newMapPoint,i);
            globalMap->AddMapPoint(newMapPoint);
            mapPointDictionary[point] = newMapPoint;
        } else {
            // Point in server map
            MapPoint* globalPoint = mapPointDictionary[point];
            if(globalPoint) {
                globalPoint->AddObservation(globalKeyFrame,i);
            }
            globalKeyFrame->AddMapPoint(globalPoint, i);
        }

        globalKeyFrame->mvpMapPoints[i] = mapPointDictionary[point];
        i++;
    }
}

void Server::CopyKeyFrameConnections(KeyFrame* keyframe) {

    KeyFrame* globalKeyFrame = keyFrameDictionary[keyframe];

    // Sync keyframe connections
    for(auto kfWeightPair : keyframe->mConnectedKeyFrameWeights) {
        KeyFrame* connKF = kfWeightPair.first;
        KeyFrame* globalConnection = keyFrameDictionary[connKF];
        int weight = kfWeightPair.second;

        globalKeyFrame->mConnectedKeyFrameWeights[globalConnection] = weight;
    }

    globalKeyFrame->mvpOrderedConnectedKeyFrames.reserve(keyframe->mvpOrderedConnectedKeyFrames.size());
    for(KeyFrame* orderedConnection : keyframe->mvpOrderedConnectedKeyFrames) {
        KeyFrame* globalOrderedConn = keyFrameDictionary[orderedConnection];
        globalKeyFrame->mvpOrderedConnectedKeyFrames.push_back(globalOrderedConn);
    }

    globalKeyFrame->mvOrderedWeights.reserve(keyframe->mvOrderedWeights.size());
    for(int weight : keyframe->mvOrderedWeights) {
        globalKeyFrame->mvOrderedWeights.push_back(weight);
    }

    // Sync spanning tree
    if(keyframe->mpParent) {
        KeyFrame* globalParent = keyFrameDictionary[keyframe->mpParent];
        globalKeyFrame->ChangeParent(globalParent);
    }

    for(KeyFrame* child : keyframe->mspChildrens) {
        KeyFrame* globalChild = keyFrameDictionary[child];
        globalKeyFrame->mspChildrens.insert(globalChild);
    }

    // Sync loop edges
    for(KeyFrame* loopClosure : keyframe->mspLoopEdges) {
        KeyFrame* globalLoopClosure = keyFrameDictionary[loopClosure];
        globalKeyFrame->mspLoopEdges.insert(globalLoopClosure);
    }
}

void Server::FindAprilTagConnections() {
    for(KeyFrame* keyframe : globalMap->GetAllKeyFrames()) {
        if(keyframe->detectedAprilTag) {
            double timestamp = keyframe->mTimeStamp;
            std::map<double, KeyFrame*>::iterator closest_it;
            double closestDiff = std::numeric_limits<double>::infinity();;
            std::map<double, KeyFrame*>::iterator lower_it;
            double lowerTimeDiff = std::numeric_limits<double>::infinity();
            std::map<double, KeyFrame*>::iterator upper_it;
            double upperTimeDiff = std::numeric_limits<double>::infinity();;
            std::map<double, KeyFrame*>::iterator end_it;

            // Get the upper and lower bounds of the timestamp from the other sequence
            if(keyframe->sequence == SEQA) {
                lower_it = timeDictionaryB.lower_bound(timestamp);
                upper_it = timeDictionaryB.upper_bound(timestamp);
                end_it = timeDictionaryB.end();
            } else {
                lower_it = timeDictionaryA.lower_bound(timestamp);
                upper_it = timeDictionaryA.upper_bound(timestamp);
                end_it = timeDictionaryA.end();
            }

            // Get the absolute time difference for both bounds
            if(lower_it != end_it) {
                lowerTimeDiff = std::abs(lower_it->second->mTimeStamp - timestamp);
            }
            if(upper_it != end_it) {
                upperTimeDiff = std::abs(upper_it->second->mTimeStamp - timestamp);
            }

            // Set closest keyframe to the lowerest difference
           if(lowerTimeDiff <= upperTimeDiff) {
                closest_it = lower_it;
                closestDiff = lowerTimeDiff;
            } else if(upperTimeDiff < lowerTimeDiff) {
                closest_it = upper_it;
                closestDiff = upperTimeDiff;
            }

            // Keyframes must be close in time
            if(closestDiff < 0.1 && closest_it != end_it) {
                // Set keyframe to the closest keyframe in time
                keyframe->aprilTagKeyFrame = closest_it->second;
            }
        }
    }
}

void Server::RegisterClient(System* client) {
    clients.push_back(client);

    // Inform client of server
    client->RegisterServer(this);
}


}