#include <charconv>
#include <iostream>
#include "plazs/gazetteer.h"

/** Print native search/lookup results for testing or shell use; IDs remain numeric WOF IDs. */
int main(int argc, char** argv)
{
    try {
        if (argc != 4) {
            std::cerr << "Usage: plazs-query DATABASE search|id VALUE\n";
            return 2;
        }
        plazs::Gazetteer database(argv[1]);
        std::vector<plazs::Place> places;
        if (std::string_view(argv[2]) == "search") {
            places = database.search(argv[3]);
        }
        else if (std::string_view(argv[2]) == "id") {
            std::string_view value(argv[3]);
            int64_t id;
            auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
            if (error != std::errc() || end != value.data() + value.size())
                throw std::invalid_argument("Expected a numeric WOF ID");
            if (auto place = database.find(id))
                places.push_back(std::move(*place));
        }
        else {
            throw std::invalid_argument("Expected search or id");
        }
        auto output = nlohmann::json::array();
        for (auto const& place : places) {
            nlohmann::json row = {
                {"id", place.id},
                {"name", place.name},
                {"countryCode", place.countryCode},
                {"placeType", place.placeType},
                {"position", place.position},
                {"bounds", place.bounds},
                {"geometryAvailable", place.geometryAvailable}};
            if (place.population)
                row["population"] = *place.population;
            if (place.geometry)
                row["geometry"] = *place.geometry;
            output.push_back(std::move(row));
        }
        std::cout << output.dump() << '\n';
        return 0;
    }
    catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
