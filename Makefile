NAME		:= webserv

VERSION		:= $(shell grep -oP 'SELFSERV_VERSION "\K[^"]+' include/selfserv.h)

# **************************************************************************** #
#    Dependencies                                                              #
# **************************************************************************** #

LIBS		:= \

INC_DIR		:= include

INCS		:= \
	$(INC_DIR) \

# **************************************************************************** #
#    Sources                                                                   #
# **************************************************************************** #

SRC_DIR		:= src

SRCS		:= $(shell find $(SRC_DIR) -name '*.cpp')

# **************************************************************************** #
#    Build                                                                     #
# **************************************************************************** #

BUILD_DIR	:= build

OBJS		:= $(SRCS:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
DEPS		:= $(OBJS:.o=.d)

CXX			:= c++
CXXFLAGS	:= -std=c++98 -Wall -Wextra -Werror -pedantic

CPPFLAGS	:= $(addprefix -I,$(INCS)) -MMD -MP
LDFLAGS		:= $(addprefix -L,$(dir $(LIBS)))
LDLIBS		:= \

# **************************************************************************** #
#    Misc                                                                      #
# **************************************************************************** #

RM			:= rm -f
MAKEFLAGS	+= --no-print-directory

RED			:= $(shell tput setaf 1)
GREEN		:= $(shell tput setaf 2)
YELLOW		:= $(shell tput setaf 3)
BLUE		:= $(shell tput setaf 4)
MAGENTA		:= $(shell tput setaf 5)
CYAN		:= $(shell tput setaf 6)
WHITE		:= $(shell tput setaf 7)
GRAY		:= $(shell tput setaf 8)
ERROR		:= $(shell tput setab 1)$(WHITE)
SUCCESS		:= $(shell tput setab 2)$(WHITE)
WARNING		:= $(shell tput setab 3)$(WHITE)
INFO		:= $(shell tput setab 4)$(WHITE)
BOLD		:= $(shell tput bold)
RESET		:= $(shell tput sgr0)
CLEAR		:= $(shell tput cuu1; tput el)
TITLE		:= $(YELLOW)$(basename $(NAME))$(RESET)

define message
	$(info [$(TITLE)] $(3)$(1)$(RESET) $(2))
endef

ifdef WITH_DEBUG
	TITLE	+= $(MAGENTA)debug$(RESET)
	CFLAGS	+= -g3
else
	CFLAGS	+= -O3
endif

ifdef WITH_SANITIZER
	TITLE	+= $(MAGENTA)sanitizer$(RESET)
	CFLAGS	+= -fsanitize=address,undefined
endif

# **************************************************************************** #
#    Targets                                                                   #
# **************************************************************************** #

.PHONY: all
all: $(NAME) ## Build the program

.PHONY: debug
debug: ## Build the program with debug symbols
	$(MAKE) WITH_DEBUG=1 all

.PHONY: sanitizer
sanitizer: ## Build the program with debug symbols and sanitizer
	$(MAKE) WITH_DEBUG=1 WITH_SANITIZER=1 all

.PHONY: loose
loose: ## Build the program ignoring warnings
	$(MAKE) CFLAGS="$(filter-out -Werror,$(CFLAGS))" all

$(NAME): $(LIBS) $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $(BUILD_DIR)/$(NAME)
	$(call message,CREATED,$(NAME),$(BLUE))

$(LIBS):
	$(MAKE) -C $(@D) -j4

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
	-printf $(CLEAR)
	$(call message,CREATED,$(basename $(notdir $@)),$(GREEN))

.PHONY: clean
clean: ## Remove all generated object files
	for lib in $(dir $(LIBS)); do $(MAKE) -C $$lib clean; done
	$(RM) -r $(BUILD_DIR)
	$(call message,DELETED,$(BUILD_DIR),$(RED))

.PHONY: fclean
fclean: clean ## Remove all generated files
	for lib in $(dir $(LIBS)); do $(MAKE) -C $$lib fclean; done
	$(RM) $(NAME)
	$(call message,DELETED,$(NAME),$(RED))

.PHONY: re
re: ## Rebuild the program
	$(MAKE) fclean
	$(MAKE) all

run.%: $(NAME) ## Run the program (usage: make run[.<arguments>])
	$(call message,RUNNING,./$(NAME) $*,$(CYAN))
	./$(NAME) $*

.PHONY: run
.IGNORE: run
run: $(NAME)
	$(call message,RUNNING,./$(NAME),$(CYAN))
	$(BUILD_DIR)/$(NAME)

valgrind.%: $(NAME) ## Run valgrind on the program (usage: make valgrind[.<arguments>])
	$(call message,RUNNING,valgrind ./$(NAME) $*,$(CYAN))
	valgrind \
	--leak-check=full \
	--show-leak-kinds=all \
	--track-origins=yes \
	--track-fds=yes \
	./$(NAME) $*

.PHONY: valgrind
valgrind: $(NAME)
	$(call message,RUNNING,valgrind ./$(NAME),$(CYAN))
	valgrind \
	--leak-check=full \
	--show-leak-kinds=all \
	--track-origins=yes \
	--track-fds=yes \
	./$(NAME)

.PHONY: norm
norm: ## Check the norm
	norminette -R CheckForbiddenSourceHeader

.PHONY: format
format: ## Format the code
	clang-format -i \
	$(shell find $(SRC_DIR) $(INC_DIR) -name '*.c' -or -name '*.cpp' -or -name '*.h' -or -name '*.hpp')

.PHONY: format.norm
format.norm: ## Format the code according to the norm
	c_formatter_42 \
	$(shell find $(SRC_DIR) $(INC_DIR) -name '*.c' -or -name '*.cpp' -or -name '*.h' -or -name '*.hpp')

.PHONY: test
test: ## TODO: Run the tests
	$(info $(INFO)TODO$(RESET) Run the tests)

.PHONY: index
index: ## Generate `compile_commands.json`
	compiledb --no-build make

.PHONY: docs
docs: ## Generate the documentation
	doxygen

.PHONY: update
update: ## Update the repository and its submodules
	git stash
	git pull
	git submodule update --init
	git stash pop

print.%: ## Print the value of a variable (usage: make print.<variable>)
	$(info '$*'='$($*)')

info.%: ## Print the target recipe (usage: make info.<target>)
	$(MAKE) --dry-run --always-make $* | grep -v "info"

force.%: ## Force execution of a target recipe (usage: make re.<target>)
	$(MAKE) --always-make $*

docker.%: ## Run a target inside a container (usage: make docker.<target>)
	docker compose run --rm make $*

.PHONY: version
version: ## Print the current version of the project
	$(info $(VERSION))

.PHONY: help
.IGNORE: help
help: ## Show this message
	echo "$(BOLD)$(TITLE)$(RESET) $(GRAY)(v$(VERSION))$(RESET)"
	echo
	echo "$(BOLD)Usage: make [<name>=<value>...]$(RESET) $(BOLD)$(CYAN)[target...]$(RESET)"
	echo
	echo "$(BOLD)Targets:$(RESET)"
	grep -E '^[a-zA-Z_.%-]+:.*?## .*$$' Makefile \
	| awk 'BEGIN {FS = ":.*?## "}; {printf "%2s$(CYAN)%-20s$(RESET) %s\n", "", $$1, $$2}'

.DEFAULT_GOAL := all
.SILENT:
.DELETE_ON_ERROR:

-include $(DEPS)
