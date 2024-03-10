#include <iostream>
#include <fstream>
#include <mpi.h>
#include <pthread.h>
#include <vector>
#include <utility>
#include <string.h>
#include <cstdlib>
using namespace std;

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define INIT_TAG 0
#define FILE_INFO_TAG 1
#define SWARM_INFO_TAG 2
#define DOWNLOAD_DONE_TAG 3
#define UPLOAD_TH_TAG 4
#define ACK_TAG 5
#define UPDATE_TAG 6


// structure representing a file (filename + list of hashes/segments)
typedef struct {
    char name[MAX_FILENAME];
    vector<string> segments;
}file;

// stucture representing a file a peer wants to download (filename, needed hashes,
// swarm(ranks of peers and their owned hash list))
typedef struct {
    char name[MAX_FILENAME];
    vector<string> wanted_segs;
    map<int, vector<string>> swarm;
}wanted_file;

//structure representing the info associated to each peer (owned files +
// files to download)
typedef struct {
    vector<file> owned_files;
    vector<wanted_file> wanted_files;
}peer_info;


peer_info info;  // information associated to a peer
map<string, pair<vector<string>, map<int, vector<string>>>> db;  // tracker database
vector<pair<string, int>> received_segs;  // segments received by a peer


// function for receiving data about the files a peer wants to download from the
// tracker (lists of hashes), the request contains, in order, the length of the
// filename and the filename, and the response contains the list of segments (hashes)
void get_file_info() {
    MPI_Status status;
    // send requests to the tracker for each wanted file
    for (int i = 0; i < info.wanted_files.size(); i++) { 
        int name_len = strlen(info.wanted_files[i].name);
        MPI_Send(nullptr, 0, MPI_INT, TRACKER_RANK, FILE_INFO_TAG, MPI_COMM_WORLD);
        MPI_Send(&name_len, 1, MPI_INT, TRACKER_RANK, FILE_INFO_TAG, MPI_COMM_WORLD);
        MPI_Send(&info.wanted_files[i].name, name_len, MPI_CHAR, TRACKER_RANK, FILE_INFO_TAG, MPI_COMM_WORLD);

        // wait for the requested data form the tracker (list of hashes)
        int seg_count;
        char segment[HASH_SIZE];
        MPI_Recv(&seg_count, 1, MPI_INT, TRACKER_RANK, FILE_INFO_TAG, MPI_COMM_WORLD, &status);

        for (int j = 0; j < seg_count; j++) {
            MPI_Recv(&segment, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, FILE_INFO_TAG, MPI_COMM_WORLD, &status);
            info.wanted_files[i].wanted_segs.push_back(segment);
        }
    }
}

// function for requesting and receiving swarm info about the files a peer wants
// to download (ranks of owner and each one's list of hashes); the request contains,
// in order, the length of the filename and the filename, and the response contains
// the number of owners, and for each owner: its rank, the number of owned segments
// and the list f segmnets (hashes)
void get_swarm_info() {
    MPI_Status status;
    for (int i = 0; i < info.wanted_files.size(); i++) {
        // request info from tracker
        int name_len = strlen(info.wanted_files[i].name);
        MPI_Send(nullptr, 0, MPI_INT, TRACKER_RANK, SWARM_INFO_TAG, MPI_COMM_WORLD);
        MPI_Send(&name_len, 1, MPI_INT, TRACKER_RANK, SWARM_INFO_TAG, MPI_COMM_WORLD);
        MPI_Send(&info.wanted_files[i].name, name_len, MPI_CHAR, TRACKER_RANK, SWARM_INFO_TAG, MPI_COMM_WORLD);

        // receive info from tracker
        int member_count;
        char segment[HASH_SIZE];
        MPI_Recv(&member_count, 1, MPI_INT, TRACKER_RANK, SWARM_INFO_TAG, MPI_COMM_WORLD, &status);

        for (int j = 0; j < member_count; j++) {
            int seg_count, rank;
            MPI_Recv(&rank, 1, MPI_INT, TRACKER_RANK, SWARM_INFO_TAG, MPI_COMM_WORLD, &status);
            MPI_Recv(&seg_count, 1, MPI_INT, TRACKER_RANK, SWARM_INFO_TAG, MPI_COMM_WORLD, &status);

            info.wanted_files[i].swarm[rank].clear();  // reset hash list
            for (int k = 0; k < seg_count; k++) {
                MPI_Recv(&segment, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, SWARM_INFO_TAG, MPI_COMM_WORLD, &status);
                info.wanted_files[i].swarm[rank].push_back(segment);
            }
        }
    }
}


// function for choosing a peer to download a segment from; its rank is chosen
// randomly from the owners of the wanted segment (defined by seg_id within the
// file_id file)
int choose_peer(int file_id, int seg_id) {
    vector<int> owners;

    // find all owners of the segment
    for (auto &member: info.wanted_files[file_id].swarm) {
        for (int i = 0; i < member.second.size(); i++) {
            if (member.second[i] == info.wanted_files[file_id].wanted_segs[seg_id]) {
                owners.push_back(member.first);
                break;
            }
        }
    }

    int index = rand() % owners.size();
    return owners[index];
}

// function for sending updates about received segments to the tracker; the
// message consists of the number of newly downloaded segments and for each
// segment: the length of the name of the file it's in, the filename, and
// the index of the segment within the file
void reset_received_segs() {
    int len = received_segs.size();
    MPI_Send(&len, 1, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);

    char filename[MAX_FILENAME];
    for (int i = 0; i < len; i++) {
        int filelen = received_segs[i].first.size();
        MPI_Send(&filelen, 1, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);
        strcpy(filename, received_segs[i].first.c_str());
        MPI_Send(&filename, filelen, MPI_CHAR, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);
        MPI_Send(&received_segs[i].second, 1, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);  //seg id
    }

    received_segs.clear();
}

// function for sending updated info about newly received segments and receiving
// updated swarm info from the tracker
void renew_info() {
    MPI_Send(nullptr, 0, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);

    reset_received_segs();    

    get_swarm_info();
}

// funtion for signaling to the tracker that the peer finished all downloads +
// sending the last updated info about owned segments
void send_finish_msg() {
    MPI_Send(nullptr, 0, MPI_INT, TRACKER_RANK, DOWNLOAD_DONE_TAG, MPI_COMM_WORLD);

    reset_received_segs();
}

// function for saving the hash list of a file in a text file "client(peer_rank)_(filename)"
void save_file(int rank, int file_id) {
    char outfile[MAX_FILENAME + 10];
    snprintf(outfile, MAX_FILENAME + 10, "client%d_%s", rank, info.wanted_files[file_id].name);

    ofstream fout(outfile);

    for (int i = 0; i < info.wanted_files[file_id].wanted_segs.size(); i++) {
        fout << info.wanted_files[file_id].wanted_segs[i] << endl;
    }

    fout.close();
}

// thread routine for the download thread of a peer; it requests info from the tracker
// about the files the peer wants to download, and for each needed segment it chooses
// a peer that owns it to send a request to; received segments are saved in the
// received_segs vector, as a pair (filename, segment_index); each time the received_segs
// vector reaches 10 elements, its contents are sent to the tracker and then it's reset
void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;

    MPI_Status status;

    get_file_info();
    get_swarm_info();

    int peer; 
    for (int i = 0; i < info.wanted_files.size(); i++) {
        for (int j = 0; j < info.wanted_files[i].wanted_segs.size(); j++) {
            peer = choose_peer(i, j);

            // request segment
            MPI_Send(nullptr, 0, MPI_INT, peer, UPLOAD_TH_TAG, MPI_COMM_WORLD);
            MPI_Send(&info.wanted_files[i].wanted_segs[j], HASH_SIZE + 1, MPI_CHAR, peer, UPLOAD_TH_TAG, MPI_COMM_WORLD);
        
            // receive response
            MPI_Recv(nullptr, 0, MPI_INT, peer, ACK_TAG, MPI_COMM_WORLD, &status);

            received_segs.push_back(make_pair(info.wanted_files[i].name, j));
            if (received_segs.size() == 10) {
                renew_info();
            }
        }

        // save each file on download completion, and send the last downloaded segments to the tracker
        save_file(rank, i);
        renew_info();
    }

    send_finish_msg();  // send finish signal to the tracker
    return NULL;
}


// thread routine for the upload thread of a peer; in a continuous loop it waits for
// empty messages with the UPLOAD_TH_TAG; a message received from a peer represents
// a request for a segment, to which it responds with an ACK_TAG to simulate sending
// that segment; a message received from the tracker means all peers have finished
// downloading and the thread should halt
void *upload_thread_func(void *arg)
{
    int rank = *(int*) arg;

    MPI_Status status;
    char segment[HASH_SIZE];
    int peer;

    while (true) {
        MPI_Recv(nullptr, 0, MPI_INT, MPI_ANY_SOURCE, UPLOAD_TH_TAG, MPI_COMM_WORLD, &status);

        switch (status.MPI_SOURCE) {
            case TRACKER_RANK:
                return NULL;  // finish thread
                break;
            default:
                peer = status.MPI_SOURCE;
                // received requested segment hash
                MPI_Recv(&segment, HASH_SIZE + 1, MPI_CHAR, peer, UPLOAD_TH_TAG, MPI_COMM_WORLD, &status);
                MPI_Send(nullptr, 0, MPI_INT, peer, ACK_TAG, MPI_COMM_WORLD);  // send ack
                break;
        }
    }

    return NULL;
}


// function checking whether a file was already added to the tracker's database
// based on it name
bool is_in_database(string filename) {
    return db.find(filename) != db.end();
}


// function for initialising the tracker's database with info received from
// all peers about the files they own; for each peer it receives, in order:
// the number of owned files, and for each file the length of the name, the
// name, number of segments, and the hashes representing the segments
void init_tracker(int numtasks) {
    int file_count;
    MPI_Status status;
    for (int i = 1; i < numtasks; i++) {  
        MPI_Recv(&file_count, 1, MPI_INT, i, INIT_TAG, MPI_COMM_WORLD, &status);
        
        for (int j = 0; j < file_count; j++) {
            int len, seg_count;
            char filename[MAX_FILENAME], segment[HASH_SIZE];
            MPI_Recv(&len, 1, MPI_INT, i, INIT_TAG, MPI_COMM_WORLD, &status);
            MPI_Recv(&filename, len, MPI_CHAR, i, INIT_TAG, MPI_COMM_WORLD, &status);
            MPI_Recv(&seg_count, 1, MPI_INT, i, INIT_TAG, MPI_COMM_WORLD, &status);

            bool already_saved = is_in_database(filename);
            for (int j = 0; j < seg_count; j++) {
                MPI_Recv(&segment, HASH_SIZE + 1, MPI_CHAR, i, INIT_TAG, MPI_COMM_WORLD, &status);
                if (!already_saved) {
                    db[filename].first.push_back(segment);  // add hash to hash list of file
                }
                db[filename].second[i].push_back(segment);  // add hash to list of owned hashes
                                                            // for the peer with rank i
            }
        }
    }
}

// function for sending the list of hashes associated with each file in the
// tracker's database to the peer demanding it; a request is received with
// the length if the filename and then the filename of the wanted file, and
// the response contains the list of its hashes
void send_file_info(int rank) {
    MPI_Status status;
    int name_len;
    char filename[MAX_FILENAME];
    char hash[HASH_SIZE];
    MPI_Recv(&name_len, 1, MPI_INT, rank, FILE_INFO_TAG, MPI_COMM_WORLD, &status);
    MPI_Recv(&filename, name_len, MPI_CHAR, rank, FILE_INFO_TAG, MPI_COMM_WORLD, &status);

    int len = db[filename].first.size();
    MPI_Send(&len, 1, MPI_INT, rank, FILE_INFO_TAG, MPI_COMM_WORLD);
    for (int i = 0; i < len; i++) {
        strcpy(hash, db[filename].first[i].c_str());
        MPI_Send(&hash, HASH_SIZE + 1, MPI_CHAR, rank, FILE_INFO_TAG, MPI_COMM_WORLD);
    }
}

// function for sending swarm info to a peer requesting it; the request contains
// the length of the filename and then the filename of the wanted file, and the
// response contains the number of segment owners for that file, and for each
// owner its rank, the number of owned hashes and the list of owned hashes
void send_swarm_info(int rank) {
    MPI_Status status;
    int name_len;
    char filename[MAX_FILENAME];
    char hash[HASH_SIZE];
    MPI_Recv(&name_len, 1, MPI_INT, rank, SWARM_INFO_TAG, MPI_COMM_WORLD, &status);
    MPI_Recv(&filename, name_len, MPI_CHAR, rank, SWARM_INFO_TAG, MPI_COMM_WORLD, &status);

    int member_count = db[filename].second.size();
    int len;
    MPI_Send(&member_count, 1, MPI_INT, rank, SWARM_INFO_TAG, MPI_COMM_WORLD);
    for (auto &member: db[filename].second) {
        len = member.second.size();
        MPI_Send(&member.first, 1, MPI_INT, rank, SWARM_INFO_TAG, MPI_COMM_WORLD);
        MPI_Send(&len, 1, MPI_INT, rank, SWARM_INFO_TAG, MPI_COMM_WORLD);
        for (int j = 0; j < member.second.size(); j++) {
            strcpy(hash, member.second[j].c_str());
            MPI_Send(&hash, HASH_SIZE + 1, MPI_CHAR, rank, SWARM_INFO_TAG, MPI_COMM_WORLD);
        }
    }
}

// function for receiving newly downloaded segments from each peer; the received info
// comes as: the number of new segments received, and for each one: the length of the
// filename the filename, and the index of the segment within the file
void update_info(int rank) {
    MPI_Status status;
    int len;
    MPI_Recv(&len, 1, MPI_INT, rank, UPDATE_TAG, MPI_COMM_WORLD, &status);

    for (int i = 0; i < len; i++) {
        int filelen, seg_id;
        char filename[MAX_FILENAME];
        MPI_Recv(&filelen, 1, MPI_INT, rank, UPDATE_TAG, MPI_COMM_WORLD, &status);
        MPI_Recv(&filename, filelen, MPI_CHAR, rank, UPDATE_TAG, MPI_COMM_WORLD, &status);
        MPI_Recv(&seg_id, 1, MPI_INT, rank, UPDATE_TAG, MPI_COMM_WORLD, &status);

        // update database (hashes owned by the peer with "rank")
        db[filename].second[rank].push_back(db[filename].first[seg_id]);
    }
}


// function running the logic of the tracker within the BitTorrent protocol;
// it receives initial information from each peer owning a file, it sends a
// broadcast message to signal to all peers they can start downloading, and
// then it waits for messages from peers, checking the type of message by 
// the MPI tag used, untill all peers signal finishing their downloads
void tracker(int numtasks, int rank) {
    init_tracker(numtasks);

    int ok = 1;
    MPI_Bcast(&ok, 1, MPI_INT, TRACKER_RANK, MPI_COMM_WORLD);

    int finished = 0;
    MPI_Status status;
    while (finished < numtasks - 1) {
        MPI_Recv(nullptr, 0, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        
        switch (status.MPI_TAG) {
            case FILE_INFO_TAG:  // file info request
                send_file_info(status.MPI_SOURCE);
                break;
            case SWARM_INFO_TAG:  // swarm info request
                send_swarm_info(status.MPI_SOURCE);
                break;
            case UPDATE_TAG:  // updated info received
                update_info(status.MPI_SOURCE);
                break;
            case DOWNLOAD_DONE_TAG: // peer done downloading
                update_info(status.MPI_SOURCE);
                finished++;
                break;
        }
    }

    // send the finish signal to each upload thread of a peer
    for (int i = 1; i < numtasks; i++) {
        MPI_Send(nullptr, 0, MPI_INT, i, UPLOAD_TH_TAG, MPI_COMM_WORLD);
    }
}


// function for reading owned files (lists of hashes) from their respective text
// files and send the info to the tracker, as well as saving it in the
// peer_info structure associated with each peer; sent info contains, in order,
// the number of owned files, and for each file: the length of the filename, the
// filename, number of segments and the list of segments (hashes)
void init_owned_files(ifstream &fin) {
    int owned_file_count;
    fin >> owned_file_count;

    MPI_Send(&owned_file_count, 1, MPI_INT, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);

    int seg_count;
    char segment[HASH_SIZE];
    for (int i = 0; i < owned_file_count; i++) {
        file new_file;

        fin >> new_file.name >> seg_count;

        int name_len = strlen(new_file.name);
        MPI_Send(&name_len, 1, MPI_INT, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);
        MPI_Send(&new_file.name, name_len, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);
        
        MPI_Send(&seg_count, 1, MPI_INT, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);
        for (int j = 0; j < seg_count; j++) {
            fin >> segment;
            new_file.segments.push_back(segment);

            MPI_Send(&segment, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD);
        }

        info.owned_files.push_back(new_file);
    }

}

// function for reading wanted files from their respectiv text files: number of
// files and then their filenames
void init_wanted_files(ifstream &fin) {
    int wanted_file_count;
    fin >> wanted_file_count;
    for (int i = 0; i < wanted_file_count; i++) {
        wanted_file new_wanted_file;
        fin >> new_wanted_file.name;
        info.wanted_files.push_back(new_wanted_file);
    }
}

// function for initialising a peer's information (owned and wanted files)
void init_peer(int rank) {
    char in_filename[MAX_FILENAME];
    snprintf(in_filename, MAX_FILENAME, "in%d.txt", rank);

    ifstream fin(in_filename);

    init_owned_files(fin);
    init_wanted_files(fin);

    fin.close();
}


// function running the logic of a peer within the BitTorrent protocol; it
// initialises the associated peer_info structure, waits for the signal from
// the tracker to start downloading, and spawns separate upload and download
// threads, each with its own logic
void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    init_peer(rank);

    int ok;
    MPI_Bcast(&ok, 1, MPI_INT, TRACKER_RANK, MPI_COMM_WORLD);

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    // init MPI system
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
