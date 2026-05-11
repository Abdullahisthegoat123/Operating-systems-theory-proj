<a id="readme-top"></a>

[![Contributors][contributors-shield]][contributors-url]
[![Forks][forks-shield]][forks-url]
[![Stargazers][stars-shield]][stars-url]
[![Issues][issues-shield]][issues-url]
[![License][license-shield]][license-url]

<br />
<div align="center">
  <h3 align="center">Multi-User Document Editor</h3>

  <p align="center">
    A collaborative document system with advanced reader-writer synchronization
    <br />
    <a href="https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor"><strong>Explore the docs »</strong></a>
    <br />
    <br />
    <a href="https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor">View Demo</a>
    ·
    <a href="https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor/issues/new?labels=bug&template=bug-report---.md">Report Bug</a>
    ·
    <a href="https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor/issues/new?labels=enhancement&template=feature-request---.md">Request Feature</a>
  </p>
</div>

<details>
  <summary>Table of Contents</summary>
  <ol>
    <li>
      <a href="#about-the-project">About The Project</a>
      <ul>
        <li><a href="#built-with">Built With</a></li>
        <li><a href="#key-features">Key Features</a></li>
      </ul>
    </li>
    <li>
      <a href="#getting-started">Getting Started</a>
      <ul>
        <li><a href="#prerequisites">Prerequisites</a></li>
        <li><a href="#installation">Installation</a></li>
      </ul>
    </li>
    <li><a href="#usage">Usage</a></li>
    <li><a href="#system-architecture">System Architecture</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#license">License</a></li>
    <li><a href="#contact">Contact</a></li>
    <li><a href="#acknowledgments">Acknowledgments</a></li>
  </ol>
</details>

## About The Project

The Multi-User Document Editor is a sophisticated collaborative document system that implements advanced reader-writer synchronization mechanisms. Multiple readers can access content simultaneously while writers get exclusive access, preventing conflicts and ensuring data integrity. The system uses POSIX file locks, semaphores, and shared memory to coordinate access between concurrent processes.

This project simulates real-world collaborative platforms like Google Docs, demonstrating fundamental concepts in operating systems including process synchronization, inter-process communication, and priority-based scheduling.

### Key Features

- **Concurrent Read Access**: Multiple users can read the document simultaneously without blocking each other
- **Exclusive Write Access**: Writers get exclusive locks to prevent data corruption
- **Priority-Based System**: Three-tier priority system (Owner > High Priority > Low Priority)
- **Owner Override**: Administrative users can force-take control with countdown warnings
- **Time-Limited Editing**: Configurable time allocations prevent users from monopolizing the document
- **Document History**: Push/pop snapshot system for version control
- **User Management**: Complete CRUD operations for user accounts with customizable permissions
- **Access Control**: Granular permissions (read-only, write-only, or both)
- **Signal-Based Communication**: Real-time notifications using UNIX signals

<p align="right">(<a href="#readme-top">back to top</a>)</p>

### Built With

* [![C][C-badge]][C-url]
* POSIX File Locking
* POSIX Semaphores
* System V Shared Memory
* ncurses (for terminal UI)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Getting Started

### Prerequisites

Ensure you have the following installed on your Linux system:

* GCC compiler
  ```sh
  sudo apt-get install gcc
  ```
* ncurses development library
  ```sh
  sudo apt-get install libncurses5-dev libncursesw5-dev
  ```
* POSIX threads library (usually pre-installed)

### Installation

1. Clone the repository
   ```sh
   git clone https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor.git
   ```

2. Navigate to the project directory
   ```sh
   cd CS2006-Multi-User-Document-Editor
   ```

3. Compile the owner program
   ```sh
   gcc -o owner owner.c shared.c -lpthread -lrt
   ```

4. Compile the user program
   ```sh
   gcc -o user user.c shared.c -lpthread -lrt
   ```

5. Compile the ncurses-based editor (optional)
   ```sh
   gcc -o myapp myapp.c -lncurses
   ```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Usage

### Starting the System

1. **Start the owner/admin program first** (this initializes shared resources):
   ```sh
   ./owner
   ```

2. **Add users** through the owner menu (option 3):
   - Enter username
   - Set priority (0 for high, 1 for low)
   - Set access type (1: read-only, 2: write-only, 3: both)

3. **Start user sessions** in separate terminals:
   ```sh
   ./user <username>
   ```

### Owner Menu Options

1. View document (read)
2. Edit document (write)
3. Add user
4. Remove user
5. Update user access
6. List all users
7. Push history snapshot
8. Pop last snapshot
9. View history log
10. Exit

### User Menu Options

1. View document (if read access granted)
2. Edit document (if write access granted)
3. Exit

### Example Workflow

```sh
# Terminal 1: Start owner
./owner

# Add users through menu
# 3 -> add user -> "alice" -> priority: 0 -> access: 3
# 3 -> add user -> "bob" -> priority: 1 -> access: 1

# Terminal 2: Alice logs in
./user alice

# Terminal 3: Bob logs in
./user bob

# Alice can read and write, Bob can only read
```

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## System Architecture

### Synchronization Mechanisms

The system uses three primary synchronization tools:

1. **POSIX File Locks** (`fcntl`): Provides the fundamental locking mechanism for the shared document file
2. **POSIX Named Semaphores**: Coordinates access between processes
   - `access_sem`: Controls general access to the document
   - `owner_sem`: Manages owner priority override
3. **Shared Memory**: Maintains lock state information across all processes
   - Current lock holder PID
   - Lock type (read/write)
   - Reader count
   - Owner waiting flag
   - Countdown and time limit information

### Priority System

- **Owner Priority (-1)**: Can force-take control with 5-second countdown, gets 30 seconds editing time
- **High Priority (0)**: Gets preferential access over low priority users, 15 seconds editing time
- **Low Priority (1)**: Standard access, 10 seconds editing time

### Reader-Writer Problem Solution

The implementation solves the classic reader-writer synchronization problem with these characteristics:

- Multiple readers can access simultaneously
- Writers get exclusive access
- Owner can override any current access with priority signaling
- No starvation through timeout mechanisms
- Fair scheduling based on priority levels

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Contributing

Contributions are what make the open source community such an amazing place to learn, inspire, and create. Any contributions you make are **greatly appreciated**.

If you have a suggestion that would make this better, please fork the repo and create a pull request. You can also simply open an issue with the tag "enhancement".

1. Fork the Project
2. Create your Feature Branch (`git checkout -b feature/AmazingFeature`)
3. Commit your Changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the Branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

### Contributors

- **Sidhart Sami** - *Project Lead & Core Development*
- **Aliyan Munawwar** - *Synchronization Implementation*
- **Hadi Armaghan** - *User Management System*
- **Zubair Ahmed** - *Testing & Documentation*

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## License

Distributed under the MIT License. See `LICENSE.txt` for more information.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Contact

Sidhart Sami - sidhart.sami@gmail.com

Project Link: [https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor](https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Acknowledgments

- [POSIX Threads Programming](https://computing.llnl.gov/tutorials/pthreads/)
- [Linux System Programming](https://man7.org/linux/man-pages/)
- [The Little Book of Semaphores](https://greenteapress.com/wp/semaphores/)
- [Operating Systems: Three Easy Pieces](https://pages.cs.wisc.edu/~remzi/OSTEP/)
- [Advanced Programming in the UNIX Environment](https://www.apuebook.com/)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

[contributors-shield]: https://img.shields.io/github/contributors/sidhartsami/CS2006-Multi-User-Document-Editor.svg?style=for-the-badge
[contributors-url]: https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor/graphs/contributors
[forks-shield]: https://img.shields.io/github/forks/sidhartsami/CS2006-Multi-User-Document-Editor.svg?style=for-the-badge
[forks-url]: https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor/network/members
[stars-shield]: https://img.shields.io/github/stars/sidhartsami/CS2006-Multi-User-Document-Editor.svg?style=for-the-badge
[stars-url]: https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor/stargazers
[issues-shield]: https://img.shields.io/github/issues/sidhartsami/CS2006-Multi-User-Document-Editor.svg?style=for-the-badge
[issues-url]: https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor/issues
[license-shield]: https://img.shields.io/github/license/sidhartsami/CS2006-Multi-User-Document-Editor.svg?style=for-the-badge
[license-url]: https://github.com/sidhartsami/CS2006-Multi-User-Document-Editor/blob/master/LICENSE.txt
[C-badge]: https://img.shields.io/badge/C-00599C?style=for-the-badge&logo=c&logoColor=white
[C-url]: https://en.wikipedia.org/wiki/C_(programming_language)
