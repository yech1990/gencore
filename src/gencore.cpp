#include "gencore.h"
#include "bamutil.h"
#include "jsonreporter.h"
#include "htmlreporter.h"

Gencore::Gencore(Options *opt){
    mOptions = opt;
    mBamHeader = NULL;
    mOutSam = NULL;
    mPreStats = new Stats(opt);
    mPostStats = new Stats(opt);
    mOutSetCleared = false;
    mProcessedTid = -1;
    mProcessedPos = -1;
}

Gencore::~Gencore(){
    outputOutSet();
    releaseClusters(mProperClusters);
    releaseClusters(mUnProperClusters);
    if(mBamHeader != NULL) {
        bam_hdr_destroy(mBamHeader);
        mBamHeader = NULL;
    }
    if(mOutSam != NULL) {
        if (sam_close(mOutSam) < 0) {
            cerr << "ERROR: failed to close " << mOutput << endl;
            exit(-1);
        }
    }
    delete mPreStats;
    delete mPostStats;
}

void Gencore::report() {
    JsonReporter jsonreporter(mOptions);
    jsonreporter.report(mPreStats, mPostStats);
    HtmlReporter htmlreporter(mOptions);
    htmlreporter.report(mPreStats, mPostStats);
}

void Gencore::releaseClusters(map<int, map<int, map<long, Cluster*>>>& clusters) {
    map<int, map<int, map<long, Cluster*>>>::iterator iter1;
    map<int, map<long, Cluster*>>::iterator iter2;
    map<long, Cluster*>::iterator iter3;
    for(iter1 = clusters.begin(); iter1 != clusters.end(); iter1++) {
        for(iter2 = iter1->second.begin(); iter2 != iter1->second.end(); iter2++) {
            for(iter3 = iter2->second.begin(); iter3 != iter2->second.end(); iter3++) {
                delete iter3->second;
            }
        }
    }
}

void Gencore::dumpClusters(map<int, map<int, map<long, Cluster*>>>& clusters) {
    map<int, map<int, map<long, Cluster*>>>::iterator iter1;
    map<int, map<long, Cluster*>>::iterator iter2;
    map<long, Cluster*>::iterator iter3;
    for(iter1 = clusters.begin(); iter1 != clusters.end(); iter1++) {
        for(iter2 = iter1->second.begin(); iter2 != iter1->second.end(); iter2++) {
            for(iter3 = iter2->second.begin(); iter3 != iter2->second.end(); iter3++) {
                 iter3->second->dump();
            }
        }
    }
}

void Gencore::outputOutSet() {
    set<bam1_t*, bamComp>::iterator iter;
    for(iter = mOutSet.begin(); iter!=mOutSet.end(); iter++) {
        writeBam(*iter);
        // delete this bam
        bam_destroy1(*iter);
    }
    mOutSet.clear();
    mOutSetCleared = true;
}

void Gencore::writeBam(bam1_t* b) {
    static int lastTid = -1;
    static int lastPos = -1;
    //BamUtil::dump(b);
    if(b->core.tid <lastTid || (b->core.tid == lastTid && b->core.pos <lastPos)) {
        // skip the -1:-1, which means unmapped
        if(b->core.tid >=0 && b->core.pos >= 0) {
            cerr << "ERROR: the input is unsorted. Found " << b->core.tid << ":" << b->core.pos << " after " << lastTid << ":" << lastPos << endl;
            cerr << "Please sort the input first." << endl << endl;
            //BamUtil::dump(b);
            //cerr << "mProcessedTid: " << mProcessedTid << endl;
            //cerr << "mProcessedPos: " << mProcessedPos << endl;
            //dumpClusters(mProperClusters);
            exit(-1);
        }
    }
    if(sam_write1(mOutSam, mBamHeader, b) <0) {
        error_exit("Writing failed, exiting ...");
    }
    lastTid = b->core.tid;
    lastPos = b->core.pos;
}

void Gencore::outputBam(bam1_t* b, bool isLeft) {
    pair<set<bam1_t*, bamComp>::iterator,bool> ret = mOutSet.insert(b);
    // pointing to its next
    set<bam1_t*, bamComp>::iterator insertpos = ++ret.first;
    // if it's left, clear the output set less than it
    if(isLeft) {
        //BamUtil::dump(b);
        set<bam1_t*, bamComp>::iterator iter;
        // write those bam less than coming left bam
        for(iter = mOutSet.begin(); iter!=insertpos; iter++) {
            // break since the reads in mProperClusters are smaller than this one
            if((*iter)->core.tid<mProcessedTid || ((*iter)->core.tid == mProcessedTid && (*iter)->core.pos <= mProcessedPos)) {
                iter++;
                break;
            }
            writeBam(*iter);
            // delete this bam
            bam_destroy1(*iter);
        }
        // clear it
        mOutSet.erase(mOutSet.begin(), iter);
    }
}

void Gencore::outputPair(Pair* p) {
    mPostStats->addMolecule(1, p->mLeft && p->mRight);

    if(mOutSam == NULL || mBamHeader == NULL)
        return ;

    if(p->mLeft) {
        mPostStats->addRead(p->mLeft->core.l_qseq, BamUtil::getED(p->mLeft));
        mPostStats->statDepth(p->mLeft->core.tid, p->mLeft->core.pos, p->mLeft->core.l_qseq);
        outputBam(p->mLeft, true);
        p->mLeft =  NULL;
    }
    if(p->mRight) {
        mPostStats->addRead(p->mRight->core.l_qseq, BamUtil::getED(p->mRight));
        mPostStats->statDepth(p->mRight->core.tid, p->mRight->core.pos, p->mRight->core.l_qseq);
        outputBam(p->mRight, false);
        // right bam will be put in the mOutSet, so make it NULL to avoid being deleted
        p->mRight =  NULL;
    }
}

void Gencore::consensus(){
    samFile *in;
    in = sam_open(mOptions->input.c_str(), "r");
    if (!in) {
        cerr << "ERROR: failed to open " << mOptions->input << endl;
        exit(-1);
    }

    if(ends_with(mOptions->output, "sam"))
        mOutSam = sam_open(mOptions->output.c_str(), "w");
    else 
        mOutSam = sam_open(mOptions->output.c_str(), "wb");
    if (!mOutSam) {
        cerr << "ERROR: failed to open output " << mOptions->output << endl;
        exit(-1);
    }

    mBamHeader = sam_hdr_read(in);
    mOptions->bamHeader = mBamHeader;
    mPreStats->makeGenomeDepthBuf();
    mPreStats->makeBedStats();
    mPostStats->makeGenomeDepthBuf();
    mPostStats->makeBedStats(mPreStats->mBedStats);

    if (mBamHeader == NULL || mBamHeader->n_targets == 0) {
        cerr << "ERROR: this SAM file has no header " << mInput << endl;
        exit(-1);
    }
    BamUtil::dumpHeader(mBamHeader);

    if (sam_hdr_write(mOutSam, mBamHeader) < 0) {
        cerr << "failed to write header" << endl;
        exit(-1);
    }

    bam1_t *b = NULL;
    b = bam_init1();
    int r;
    int count = 0;
    int lastTid = -1;
    int lastPos = -1;
    while ((r = sam_read1(in, mBamHeader, b)) >= 0) {
        // check whether the BAM is sorted
        if(b->core.tid <lastTid || (b->core.tid == lastTid && b->core.pos <lastPos)) {
            // skip the -1:-1, which means unmapped
            if(b->core.tid >=0 && b->core.pos >= 0) {
                cerr << "ERROR: the input is unsorted. Found " << b->core.tid << ":" << b->core.pos << " after " << lastTid << ":" << lastPos << endl;
                cerr << "Please sort the input first." << endl << endl;
                BamUtil::dump(b);
                exit(-1);
            }
        }
        // for testing, we only process to some contig
        if(mOptions->maxContig>0 && b->core.tid>=mOptions->maxContig){
            b = bam_init1();
            break;
        }
        // if debug flag is enabled, show which contig we are start to process
        if(mOptions->debug && b->core.tid > lastTid) {
            cerr << "Starting contig " << b->core.tid << endl;
        }
        lastTid = b->core.tid;
        lastPos = b->core.pos;

        // unmapped reads, we just write it and continue
        if(b->core.tid < 0 || b->core.pos < 0 ) {
            // we arrived the end of bam file with unmapped reads, go clear the output set first
            if(!mOutSetCleared) {
                finishConsensus(mProperClusters);
                outputOutSet();
            }
            mPreStats->addRead(b->core.l_qseq, 0, false);
            mPostStats->addRead(b->core.l_qseq, 0, false);
            //writeBam(b);
            continue;
        }

        // for secondary alignments, we just skip it
        if(!BamUtil::isPrimary(b)) {
            continue;
        }
        mPreStats->statDepth(b->core.tid, b->core.pos, b->core.l_qseq);
        addToCluster(b);
        b = bam_init1();
    }

    
    //finishConsensus(mUnProperClusters);

    bam_destroy1(b);
    sam_close(in);

    cerr << "----Before gencore processing:" << endl;
    mPreStats->print();

    cerr << endl << "----After gencore processing:" << endl;
    mPostStats->print();

    report();
}

void Gencore::addToProperCluster(bam1_t* b) {
    int tid = b->core.tid;
    int left = b->core.pos;
    long right;

    if(b->core.mtid == b->core.tid ) { // on same contig
        if(b->core.isize < 0) {
            left = b->core.mpos;
        }
        right = left + abs(b->core.isize) -  1;
    } else { // cross contig, we only process this read, but dont process its mate
        left = b->core.pos;
        // no mate or mate is not mapped, we cannot remove duplication or make consensus read, so just write it
        if(b->core.mtid < 0) {
            outputBam(b, true);
            return;
        } else {
            right = -mBamHeader->target_len[b->core.tid] * (b->core.mtid+1) + b->core.mpos;
        }
    }

    createCluster(mProperClusters, tid, left, right);
    mProperClusters[tid][left][right]->addRead(b);


    static int tick = 0;
    tick++;
    if(tick % 10000 != 0)
        return;

    // make consensus merge
    map<int, map<int, map<long, Cluster*>>>::iterator iter1;
    map<int, map<long, Cluster*>>::iterator iter2;
    map<long, Cluster*>::iterator iter3;
    bool needBreak = false;
    // to mark the smallest tid in the set
    mProcessedTid = mBamHeader->n_targets;
    int processedPos;
    for(iter1 = mProperClusters.begin(); iter1 != mProperClusters.end();) {
        if(iter1->first > tid || needBreak)
            break;
        // to mark the smallest pos in this set
        processedPos =mBamHeader->target_len[tid];
        for(iter2 = iter1->second.begin(); iter2 != iter1->second.end(); ) {
            if(iter1->first == tid && iter2->first >= b->core.pos) {
                needBreak = true;
                break;
            }
            for(iter3 = iter2->second.begin(); iter3 != iter2->second.end(); ) {
                // only deal with the clusters with right < processing pos
                if(iter1->first == tid && iter3->first >= b->core.pos) {
                    break;
                }
                vector<Pair*> csPairs = iter3->second->clusterByUMI(mOptions->properReadsUmiDiffThreshold, mPreStats, mPostStats);
                for(int i=0; i<csPairs.size(); i++) {
                    //csPairs[i]->dump();
                    outputPair(csPairs[i]);
                    delete csPairs[i];
                }
                // this tid:left:right is done
                delete iter3->second;
                iter3 = iter2->second.erase(iter3);
            }
            // this tid:left is done
            if(iter2->second.size() == 0) {
                iter2 = iter1->second.erase(iter2);
            } else {
                if(processedPos > iter2->first)
                    processedPos = iter2->first;
                iter2++;
            }
        }
        // this tid is done
        if(iter1->second.size() == 0) {
            iter1 = mProperClusters.erase(iter1);
        } else {
            if(mProcessedTid > iter1->first) {
                mProcessedTid = iter1->first;
                mProcessedPos = processedPos;
            }
            iter1++;
        }
    }
}

void Gencore::finishConsensus(map<int, map<int, map<long, Cluster*>>>& clusters) {
    // make consensus merge
    map<int, map<int, map<long, Cluster*>>>::iterator iter1;
    map<int, map<long, Cluster*>>::iterator iter2;
    map<long, Cluster*>::iterator iter3;
    for(iter1 = clusters.begin(); iter1 != clusters.end();) {
        for(iter2 = iter1->second.begin(); iter2 != iter1->second.end(); ) {
            for(iter3 = iter2->second.begin(); iter3 != iter2->second.end(); ) {
                // for unmapped reads, we just store them
                if(iter1->first < 0 || iter2->first < 0 || iter3->first < 0 ) {
                    map<string, Pair*>::iterator iterOfPairs;
                    for(iterOfPairs = iter3->second->mPairs.begin(); iterOfPairs!=iter3->second->mPairs.end(); iterOfPairs++) {
                        //csPairs[i]->dump();
                        outputPair(iterOfPairs->second);
                        delete iterOfPairs->second;
                    }
                } else {
                    vector<Pair*> csPairs = iter3->second->clusterByUMI(mOptions->unproperReadsUmiDiffThreshold, mPreStats, mPostStats);
                    for(int i=0; i<csPairs.size(); i++) {
                        //csPairs[i]->dump();
                        outputPair(csPairs[i]);
                        delete csPairs[i];
                    }
                }
                // this tid:left:right is done
                delete iter3->second;
                iter3 = iter2->second.erase(iter3);
            }
            // this tid:left is done
            if(iter2->second.size() == 0) {
                iter2 = iter1->second.erase(iter2);
            } else {
                iter2++;
            }
        }
        // this tid is done
        if(iter1->second.size() == 0) {
            iter1 = clusters.erase(iter1);
        } else {
            iter1++;
        }
    }
}

void Gencore::addToUnProperCluster(bam1_t* b) {
    int tid = b->core.tid;
    int left = b->core.pos;
    long right = b->core.mpos;
    if(b->core.mtid < b->core.tid) {
        tid = b->core.mtid;
        left = b->core.mpos;
        right = b->core.pos;
    }
    createCluster(mUnProperClusters, tid, left, right);
    mUnProperClusters[tid][left][right]->addRead(b);
}

void Gencore::createCluster(map<int, map<int, map<long, Cluster*>>>& clusters, int tid, int left, long right) {
    map<int, map<int, map<long, Cluster*>>>::iterator iter1 = clusters.find(tid);

    if(iter1 == clusters.end()) {
        clusters[tid] = map<int, map<long, Cluster*>>();
        clusters[tid][left] = map<long, Cluster*>();
        clusters[tid][left][right] = new Cluster(mOptions);
    } else {
        map<int, map<long, Cluster*>>::iterator iter2  =iter1->second.find(left);
        if(iter2 == iter1->second.end()) {
            clusters[tid][left] = map<long, Cluster*>();
            clusters[tid][left][right] = new Cluster(mOptions);
        } else {
            map<long, Cluster*>::iterator iter3 = iter2->second.find(right);
            if(iter3 == iter2->second.end())
                clusters[tid][left][right] = new Cluster(mOptions);
        }
    }
}

void Gencore::addToCluster(bam1_t* b) {
    mPreStats->addRead(b->core.l_qseq, BamUtil::getED(b));
    // unproperly mapped
    if(b->core.tid < 0) {
        addToUnProperCluster(b);
    } else {
        addToProperCluster(b);
    }
}