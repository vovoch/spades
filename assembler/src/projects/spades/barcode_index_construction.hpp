#pragma once

#include "common/barcode_index/barcode_mapper.hpp"
#include "utils/logger/logger.hpp"
#include "common/pipeline/stage.hpp"

using namespace tslr_resolver;

namespace debruijn_graph {
    class BarcodeMapConstructionStage : public spades::AssemblyStage {

    public:

        BarcodeMapConstructionStage() :
                AssemblyStage("Barcode map construction", "barcode_map_construction") {
        }

        void run(debruijn_graph::conj_graph_pack &graph_pack, const char *);
        DECL_LOGGER("BarcodeMapConstrusctionStage")
    };

} //debruijn_graph
