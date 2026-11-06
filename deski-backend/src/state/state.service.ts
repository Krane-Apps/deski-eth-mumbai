import { Injectable } from '@nestjs/common';

export interface DeskiState {
  stake: boolean;
  exercise: boolean;
}

/** In-memory state for the (single) user. Resets when the server restarts. */
@Injectable()
export class StateService {
  private state: DeskiState = { stake: false, exercise: false };

  get(): DeskiState {
    return { ...this.state };
  }

  setStake(value: boolean) {
    this.state.stake = value;
  }

  setExercise(value: boolean) {
    this.state.exercise = value;
  }
}
