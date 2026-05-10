// benchmark_data_generator.cpp
// Generates 1,000,000 row sales dataset for benchmarking

#include <iostream>
#include <fstream>
#include <random>
#include <string>
#include <vector>

const int NUM_ROWS = 1000000;

const std::vector<std::string> COUNTRIES = {
    "USA", "Canada", "Mexico", "UK", "Germany", "France", "Spain", "Italy",
    "Japan", "China", "India", "Brazil", "Argentina", "Australia", "South Korea",
    "Netherlands", "Sweden", "Norway", "Switzerland", "Belgium", "Austria",
    "Denmark", "Finland", "Ireland"
};

const std::vector<std::string> CATEGORIES = {
    "Electronics", "Books", "Clothing", "Home", "Sports", "Toys", "Beauty",
    "Groceries", "Automotive", "Office", "Music", "Movies", "Games", "Software",
    "Hardware", "Tools", "Garden", "Pet", "Baby", "Health", "Jewelry", "Luggage",
    "Furniture", "Appliances", "Cameras", "Phones", "Tablets", "Laptops",
    "Desktops", "Monitors", "Printers", "Routers", "Headphones", "Speakers",
    "Watches", "Shoes", "Bags", "Hats", "Gloves", "Scarves", "Belts", "Socks",
    "Underwear", "Pajamas", "Swimwear", "Activewear", "Outerwear", "Dresses",
    "Shirts", "Pants", "Shorts", "Skirts", "Jackets", "Coats", "Sweaters",
    "Hoodies", "Jeans", "TShirts", "Polos", "TankTops", "Blouses", "Suits",
    "Ties", "Sunglasses", "Backpacks", "Wallets", "Keychains", "Stickers",
    "Posters", "Calendars", "Notebooks", "Pens", "Pencils", "Markers", "Erasers",
    "Rulers", "Scissors", "Glue", "Tape", "Staples", "Paper", "Envelopes",
    "Boxes", "Bags"
};

// Random number generators
static std::mt19937 rng(42); // Fixed seed for reproducibility
static std::uniform_int_distribution<int> date_dist(20240101, 20241231);
static std::uniform_int_distribution<int> country_dist(0, COUNTRIES.size() - 1);
static std::uniform_int_distribution<int> category_dist(0, CATEGORIES.size() - 1);
static std::uniform_int_distribution<int> product_dist(1, 10000);
static std::uniform_int_distribution<int> quantity_dist(1, 10);
static std::uniform_real_distribution<double> price_dist(10.0, 999.99);

void generateBenchmarkCSV(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Cannot create " << filename << std::endl;
        return;
    }
    
    // Write header
    file << "id,date,country,category,product_id,quantity,price\n";
    
    // Write rows
    for (int i = 1; i <= NUM_ROWS; i++) {
        int date = date_dist(rng);
        const std::string& country = COUNTRIES[country_dist(rng)];
        const std::string& category = CATEGORIES[category_dist(rng)];
        int product_id = product_dist(rng);
        int quantity = quantity_dist(rng);
        double price = price_dist(rng);
        
        file << i << "," << date << "," << country << "," << category << ","
             << product_id << "," << quantity << "," << price << "\n";
        
        if (i % 100000 == 0) {
            std::cout << "Generated " << i << " rows...\n";
        }
    }
    
    file.close();
    std::cout << "Generated " << NUM_ROWS << " rows to " << filename << std::endl;
}

int main() {
    generateBenchmarkCSV("benchmark_sales.csv");
    return 0;
}