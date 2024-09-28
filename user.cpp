//
// Created by User on 7/30/2024.
//
#include <iostream>
#include "EasySocket.hpp"

using namespace std;

class user
{
private:
    string username ,userpass ;
    //it s most get from server
    static int user_count;

public:
    user()
    {
        cout<<"pls enter username :";
        cin>>username;
        cout<<"pls enter password (6 num and 2 char (more than 8 entry)):";
        cin>>userpass;
    }
    int find_user(string user ,string pass )
    {
        //most go to server and download the user info
        //befor this also we most to check temp downloded file
    }
    friend class input;
};

class input
{
private:
    bool signup = false , login = false ;

public:
    input(bool input1)
    {

        //cin>>input1;

        if (input1 == 0)
        {
            signup = true;
            user();

        }
        if (input1 == 1)
        {
            login = true;
            string username , userpass;
            cout<<"pls enter your username";
            cin>>username;
            cout<<"pls enter your password";
            cin>>userpass;

  //          find_user(username,userpass);

        }


    }


};

int main()
{
    cout<<"hello"<<endl;
    cout<<"are you ready for safe chat?"<<endl<<"if you login enter 1 and if you want to signup enter 0"<<endl;
    bool entry = 0;
    cin>>entry;

    input user(entry);
}
