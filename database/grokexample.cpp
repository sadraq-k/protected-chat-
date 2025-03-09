//
// Created by sadraq on 3/8/25.
//
#include <iostream>
#include <sstream>

using namespace std;

int main()
{
    string name = "sadra" , username = "sadraq" , how_they_find_us = "so hard";
    stringstream query;
    query<<"INSERT INTO uers(name,username,how_they_find_us) VALUES('"<<name<<
         "' , '"<<username<<"' , '" <<how_they_find_us<<"');";

    string finalquery;

    finalquery = query.str();
    cout<<finalquery;
    return 0;
}